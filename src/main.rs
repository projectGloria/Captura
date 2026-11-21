#![windows_subsystem = "windows"]

use libloading::os::windows::Library;
use std::{
    ffi::{CString, OsString, c_char, c_void},
    fs::{self, OpenOptions},
    io::{self, BufWriter, Write},
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::{Path, PathBuf},
    slice,
    sync::atomic::{AtomicU64, Ordering},
};

unsafe extern "C" {
    fn captura_run() -> i32;
}
fn main() {
    std::process::exit(unsafe { captura_run() });
}

unsafe fn path_from_wide(ptr: *const u16) -> PathBuf {
    let mut len = 0;
    unsafe {
        while *ptr.add(len) != 0 {
            len += 1;
        }
        OsString::from_wide(slice::from_raw_parts(ptr, len)).into()
    }
}
fn output(text: impl AsRef<std::ffi::OsStr>, dest: *mut u16, capacity: usize) {
    let chars: Vec<_> = text
        .as_ref()
        .encode_wide()
        .take(capacity.saturating_sub(1))
        .chain([0])
        .collect();
    if capacity > 0 {
        unsafe {
            std::ptr::copy_nonoverlapping(chars.as_ptr(), dest, chars.len());
        }
    }
}

static SERIAL: AtomicU64 = AtomicU64::new(0);
fn save_png(folder: &Path, stem: &str, pixels: &[u8], w: u32, h: u32) -> io::Result<PathBuf> {
    if !folder.is_absolute() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Choose an absolute screenshot folder",
        ));
    }
    fs::create_dir_all(folder)?;
    let id = SERIAL.fetch_add(1, Ordering::Relaxed);
    let temp = folder.join(format!(
        ".captura-{}-{id}-{}.tmp",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos()
    ));
    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temp)?;
    let result = (|| {
        let mut buf = BufWriter::new(file);
        let mut encoder = png::Encoder::new(&mut buf, w, h);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        encoder.set_compression(png::Compression::Fast);
        let mut writer = encoder.write_header()?;
        writer.write_image_data(pixels)?;
        writer.finish()?;
        buf.flush()?;
        drop(buf);
        for n in 0.. {
            let name = if n == 0 {
                format!("{stem}.png")
            } else {
                format!("{stem} ({n}).png")
            };
            let dest = folder.join(name);
            // MoveFileW publishes a complete file and fails if its destination exists.
            unsafe extern "system" {
                fn MoveFileW(from: *const u16, to: *const u16) -> i32;
            }
            let wide = |p: &Path| p.as_os_str().encode_wide().chain([0]).collect::<Vec<_>>();
            if unsafe { MoveFileW(wide(&temp).as_ptr(), wide(&dest).as_ptr()) } != 0 {
                return Ok(dest);
            }
            let err = io::Error::last_os_error();
            if err.kind() != io::ErrorKind::AlreadyExists {
                return Err(err);
            }
        }
        unreachable!()
    })();
    if result.is_err() {
        let _ = fs::remove_file(temp);
    }
    result
}

#[unsafe(no_mangle)]
unsafe extern "C" fn captura_save(
    folder: *const u16,
    stem: *const u16,
    rgba: *const u8,
    w: u32,
    h: u32,
    dest: *mut u16,
    cap: usize,
) -> bool {
    let result = unsafe {
        save_png(
            &path_from_wide(folder),
            &path_from_wide(stem).to_string_lossy(),
            slice::from_raw_parts(rgba, w as usize * h as usize * 4),
            w,
            h,
        )
    };
    match result {
        Ok(path) => {
            output(path, dest, cap);
            true
        }
        Err(e) => {
            output(e.to_string(), dest, cap);
            false
        }
    }
}

type Engine = *mut c_void;
fn recognize(pixels: &[u8], w: i32, h: i32) -> Result<String, Box<dyn std::error::Error>> {
    let dir = std::env::current_exe()?
        .parent()
        .ok_or("Missing executable directory")?
        .join("ocr");
    let data_path = CString::new(dir.to_string_lossy().as_bytes())?;
    unsafe {
        let dll = Library::load_with_flags(dir.join("tesseract50.dll"), 0x1100)?;
        let create = dll.get::<unsafe extern "C" fn() -> Engine>(b"TessBaseAPICreate")?;
        let delete = dll.get::<unsafe extern "C" fn(Engine)>(b"TessBaseAPIDelete")?;
        let init = dll.get::<unsafe extern "C" fn(Engine, *const c_char, *const c_char) -> i32>(
            b"TessBaseAPIInit3",
        )?;
        let set = dll.get::<unsafe extern "C" fn(Engine, *const u8, i32, i32, i32, i32)>(
            b"TessBaseAPISetImage",
        )?;
        let set_mode =
            dll.get::<unsafe extern "C" fn(Engine, i32)>(b"TessBaseAPISetPageSegMode")?;
        let get =
            dll.get::<unsafe extern "C" fn(Engine) -> *mut c_char>(b"TessBaseAPIGetUTF8Text")?;
        let free = dll.get::<unsafe extern "C" fn(*mut c_char)>(b"TessDeleteText")?;
        let api = create();
        if api.is_null() {
            return Err("Cannot initialize the OCR engine".into());
        }
        let lang = CString::new("eng")?;
        let result = if init(api, data_path.as_ptr(), lang.as_ptr()) != 0 {
            Err("Cannot load the English recognition data".into())
        } else {
            let gray: Vec<u8> = pixels
                .as_chunks::<4>()
                .0
                .iter()
                .map(|p| {
                    if p[3] == 0 {
                        255
                    } else {
                        ((77 * p[0] as u32 + 150 * p[1] as u32 + 29 * p[2] as u32) >> 8) as u8
                    }
                })
                .collect();
            set(api, gray.as_ptr(), w, h, 1, w);
            set_mode(api, 11);
            let ptr = get(api);
            if ptr.is_null() {
                Err("Text recognition failed".into())
            } else {
                let s = std::ffi::CStr::from_ptr(ptr)
                    .to_string_lossy()
                    .trim()
                    .to_owned();
                free(ptr);
                Ok(s)
            }
        };
        delete(api);
        result
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn captura_ocr(rgba: *const u8, w: i32, h: i32, ok: *mut bool) -> *mut u16 {
    let result = recognize(
        unsafe { slice::from_raw_parts(rgba, w as usize * h as usize * 4) },
        w,
        h,
    );
    unsafe {
        *ok = result.is_ok();
    }
    let text = result
        .unwrap_or_else(|e| format!("Text extraction failed. {e}"))
        .replace("\r\n", "\n")
        .replace('\n', "\r\n");
    let chars: Vec<u16> = text.encode_utf16().chain([0]).collect();
    // Owned buffer returned to the UI until its result dialog closes.
    Box::into_raw(chars.into_boxed_slice()) as *mut u16
}
#[unsafe(no_mangle)]
unsafe extern "C" fn captura_free(ptr: *mut u16) {
    let mut len = 0;
    unsafe {
        while *ptr.add(len) != 0 {
            len += 1;
        }
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
            ptr,
            len + 1,
        )));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn repeated_saves_keep_both_complete_images() {
        let dir = std::env::temp_dir().join(format!("captura-test-{}", std::process::id()));
        let a = save_png(&dir, "Capture", &[21, 32, 45, 0], 1, 1).unwrap();
        let b = save_png(&dir, "Capture", &[1, 2, 3, 255], 1, 1).unwrap();
        assert_ne!(a, b);
        for (path, expected) in [(a, [21, 32, 45, 0]), (b, [1, 2, 3, 255])] {
            let mut reader = png::Decoder::new(io::BufReader::new(fs::File::open(&path).unwrap()))
                .read_info()
                .unwrap();
            let mut bytes = [0; 4];
            reader.next_frame(&mut bytes).unwrap();
            assert_eq!(bytes, expected);
            fs::remove_file(path).unwrap();
        }
        fs::remove_dir(dir).unwrap();
    }
}
