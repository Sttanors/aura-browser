use adblock::{Engine, request::Request};
use std::ffi::{CStr, CString, c_char, c_void};
unsafe fn text<'a>(s: *const c_char) -> &'a str {
    unsafe { CStr::from_ptr(s).to_str().unwrap_or("") }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_new(rules: *const c_char) -> *mut c_void {
    std::panic::catch_unwind(|| {
        Box::into_raw(Box::new(Engine::new_with_list_text(unsafe { text(rules) }))) as *mut c_void
    })
    .unwrap_or(std::ptr::null_mut())
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_free(engine: *mut c_void) {
    if !engine.is_null() {
        drop(unsafe { Box::from_raw(engine as *mut Engine) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_check(
    engine: *mut c_void,
    url: *const c_char,
    source: *const c_char,
    kind: *const c_char,
) -> bool {
    if engine.is_null() {
        return false;
    }
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let request = Request::new(
            unsafe { text(url) },
            unsafe { text(source) },
            unsafe { text(kind) },
            "get",
        );
        request.is_ok_and(|r| {
            unsafe { &*(engine as *mut Engine) }
                .check_network_request(&r)
                .should_block()
        })
    }))
    .unwrap_or(false)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_cosmetics(
    engine: *mut c_void,
    url: *const c_char,
    classes: *const c_char,
    ids: *const c_char,
) -> *mut c_char {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if engine.is_null() {
            return "[]".into();
        }
        let e = unsafe { &*(engine as *mut Engine) };
        let resources = e.url_cosmetic_resources(unsafe { text(url) });
        let mut selectors = resources.hide_selectors.clone();
        if !resources.generichide {
            let classes: Vec<String> =
                serde_json::from_str(unsafe { text(classes) }).unwrap_or_default();
            let ids: Vec<String> = serde_json::from_str(unsafe { text(ids) }).unwrap_or_default();
            selectors.extend(e.hidden_class_id_selectors(
                classes.iter().map(String::as_str),
                ids.iter().map(String::as_str),
                &resources.exceptions,
            ));
        }
        serde_json::to_string(&selectors).unwrap_or_else(|_| "[]".into())
    }))
    .unwrap_or_else(|_| "[]".into());
    CString::new(result).unwrap().into_raw()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(unsafe { CString::from_raw(value) });
    }
}
