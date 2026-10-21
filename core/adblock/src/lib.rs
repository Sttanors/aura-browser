use adblock::filters::cosmetic::{
    CosmeticFilterAction, CosmeticFilterMask, CosmeticFilterOperator,
};
use adblock::{Engine, request::Request};
use std::ffi::{CStr, CString, c_char, c_void};
fn procedural_supported(
    ops: &[CosmeticFilterOperator],
    action: Option<&CosmeticFilterAction>,
) -> bool {
    if !matches!(action, None | Some(CosmeticFilterAction::Style(_))) {
        return false;
    }
    ops.iter().all(|op| match op {
        CosmeticFilterOperator::Xpath(_) => false,
        CosmeticFilterOperator::HasText(arg) | CosmeticFilterOperator::MatchesPath(arg) => {
            !arg.trim().starts_with('/')
        }
        CosmeticFilterOperator::MatchesAttr(arg)
        | CosmeticFilterOperator::MatchesCss(arg)
        | CosmeticFilterOperator::MatchesCssBefore(arg)
        | CosmeticFilterOperator::MatchesCssAfter(arg) => !arg.contains('/'),
        CosmeticFilterOperator::MinTextLength(arg) => {
            arg.parse::<u32>().is_ok_and(|n| n <= 1_000_000)
        }
        CosmeticFilterOperator::Upward(arg) => {
            if arg.bytes().all(|c| c.is_ascii_digit()) {
                arg.parse::<u32>().is_ok_and(|n| (1..=32).contains(&n))
            } else {
                arg.len() <= 512
            }
        }
        CosmeticFilterOperator::CssSelector(_) => true,
    })
}
unsafe fn text<'a>(s: *const c_char) -> &'a str {
    unsafe { CStr::from_ptr(s).to_str().unwrap_or("") }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_new(rules: *const c_char) -> *mut c_void {
    std::panic::catch_unwind(|| {
        use adblock::resources::{MimeType, Resource, ResourceType};
        let mut engine = Engine::new_with_list_text(unsafe { text(rules) });
        let resource = |name: &str, aliases: Vec<&str>, mime, content: &str| Resource {
            name: name.into(),
            aliases: aliases.into_iter().map(String::from).collect(),
            kind: ResourceType::Mime(mime),
            content: content.into(),
            dependencies: vec![],
            permission: Default::default(),
        };
        engine.use_resources([
            resource(
                "noop.js",
                vec!["noopjs"],
                MimeType::ApplicationJavascript,
                "Ow==",
            ),
            resource("noop.css", vec!["noopcss"], MimeType::TextCss, ""),
            resource(
                "1x1.gif",
                vec!["1x1-transparent.gif", "transparent.gif"],
                MimeType::ImageGif,
                "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7",
            ),
        ]);
        Box::into_raw(Box::new(engine)) as *mut c_void
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
        let mut procedural: Vec<_> = resources
            .procedural_actions
            .iter()
            .filter_map(|text| {
                serde_json::from_str::<adblock::cosmetic_filter_cache::ProceduralOrActionFilter>(
                    text,
                )
                .ok()
            })
            .filter(|rule| procedural_supported(&rule.selector, rule.action.as_ref()))
            .take(256)
            .collect();
        procedural.sort_by_cached_key(|rule| serde_json::to_string(rule).unwrap_or_default());
        if procedural.is_empty() {
            serde_json::to_string(&selectors).unwrap_or_else(|_| "[]".into())
        } else {
            serde_json::json!({"selectors":selectors,"procedural":procedural}).to_string()
        }
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_stats(
    rules: *const c_char,
    network: *mut u64,
    cosmetic: *mut u64,
    skipped: *mut u64,
) -> bool {
    if rules.is_null() || network.is_null() || cosmetic.is_null() || skipped.is_null() {
        return false;
    }
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        use adblock::lists::{ParseOptions, ParsedLine, parse_filter};
        let Ok(rules) = unsafe { CStr::from_ptr(rules) }.to_str() else {
            return false;
        };
        let (mut n, mut c, mut s) = (0, 0, 0);
        for line in rules.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('!') || line.starts_with("[Adblock") {
                continue;
            }
            match parse_filter(line, false, ParseOptions::default()) {
                Ok(ParsedLine::Network(rule)) => {
                    if rule.is_csp() {
                        s += 1;
                    } else {
                        n += 1;
                    }
                }
                Ok(ParsedLine::Cosmetic(rule)) => {
                    if rule.mask.contains(CosmeticFilterMask::SCRIPT_INJECT)
                        || !procedural_supported(&rule.selector, rule.action.as_ref())
                    {
                        s += 1;
                    } else {
                        c += 1;
                    }
                }
                Err(_) => s += 1,
            }
        }
        unsafe {
            *network = n;
            *cosmetic = c;
            *skipped = s;
        }
        true
    }))
    .unwrap_or(false)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aura_adblock_decision(
    engine: *mut c_void,
    url: *const c_char,
    source: *const c_char,
    kind: *const c_char,
    method: *const c_char,
    rewritten: *mut *mut c_char,
) -> i32 {
    if !rewritten.is_null() {
        unsafe {
            *rewritten = std::ptr::null_mut();
        }
    }
    if engine.is_null() {
        return 0;
    }
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let Ok(request)=Request::new(unsafe{text(url)},unsafe{text(source)},unsafe{text(kind)},unsafe{text(method)}) else {return 0;};
        let result=unsafe{&*(engine as *mut Engine)}.check_network_request(&request);
        if result.should_block() {
            match result.redirect.as_deref() {
                Some("data:application/javascript;base64,Ow==")=>2,
                Some("data:text/css;base64,")=>3,
                Some("data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7")=>4,
                _=>1,
            }
        } else {
            if !rewritten.is_null() {
                if let Some(url)=result.rewritten_url {
                    if let Ok(text)=CString::new(url) {unsafe{*rewritten=text.into_raw();}}
                }
            }
            0
        }
    })).unwrap_or(0)
}
