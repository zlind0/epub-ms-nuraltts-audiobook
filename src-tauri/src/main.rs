// EPUB电子书音频生成器后端。
//
// 链路（与 aurora EbookExporter / EpubParser 同口径）：
//   EPUB解析(TOC: NCX>nav>h1h2>逐章) → 按heading切段 → 段内按<p>切块(>450字按句再切)
//   → N个 `mstts serve` 子进程组成TTS池并行合成(raw PCM 24k/mono)
//   → 段内按序拼接 → 写WAV → afconvert直出m4a(保持24k单声道,不重采样)
//   → 文件名 0001-标题.m4a,码率可调(默认64k)。

use std::collections::HashMap;
use std::io::{Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use scraper::{Html, Selector};
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter};

// ---------- 数据模型 ----------

#[derive(Debug, Clone, Serialize, Deserialize)]
struct TocEntry {
    index: usize,
    title: String,
    level: usize,
    chapter: usize,
    block: usize,
}

#[derive(Debug, Clone)]
struct TextBlock {
    text: String,
    level: usize,
}

#[derive(Debug, Clone)]
struct Chapter {
    title: String,
    blocks: Vec<TextBlock>,
}

struct BookData {
    title: String,
    author: String,
    chapters: Vec<Chapter>,
    toc: Vec<TocEntry>,
    path: String,
}

fn books() -> &'static Mutex<HashMap<String, BookData>> {
    static M: OnceLock<Mutex<HashMap<String, BookData>>> = OnceLock::new();
    M.get_or_init(|| Mutex::new(HashMap::new()))
}

fn cancelled_flag() -> &'static AtomicBool {
    static F: OnceLock<AtomicBool> = OnceLock::new();
    F.get_or_init(|| AtomicBool::new(false))
}

/// 取消通知(唤醒 select,立即中断在途合成/编码;无等待者时自动丢弃,不留旧信号)。
fn cancel_notify() -> &'static Arc<tokio::sync::Notify> {
    static N: OnceLock<Arc<tokio::sync::Notify>> = OnceLock::new();
    N.get_or_init(|| Arc::new(tokio::sync::Notify::new()))
}

#[derive(Debug, Clone, Serialize)]
struct BookInfo {
    id: String,
    title: String,
    author: String,
    toc: Vec<TocEntry>,
    total: usize,
    /// epub 文件所在目录(默认输出目录)
    dir: String,
}

#[derive(Debug, Clone, Serialize)]
struct SegmentPreview {
    title: String,
    paragraphs: Vec<String>,
    chars: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct VoiceInfo {
    id: String,
    name: String,
    locale: String,
    gender: String,
    installed: bool,
}

// ---------- EPUB 解析 ----------

fn zip_read(zip: &mut zip::ZipArchive<std::fs::File>, name: &str) -> Option<Vec<u8>> {
    let mut f = zip.by_name(name).ok()?;
    let mut b = Vec::new();
    f.read_to_end(&mut b).ok()?;
    Some(b)
}

fn normalize_zip_path(p: &str) -> String {
    let mut out: Vec<&str> = Vec::new();
    for s in p.split('/') {
        match s {
            "" | "." => {}
            ".." => {
                out.pop();
            }
            x => out.push(x),
        }
    }
    out.join("/")
}

fn resolve_entry(names: &[String], base: &str, href: &str) -> Option<String> {
    let raw = href.split('#').next().unwrap_or("").trim();
    if raw.is_empty() {
        return None;
    }
    // URL decode 简版
    let decoded = percent_decode(raw);
    let cands = if base.is_empty() {
        vec![decoded.clone()]
    } else {
        vec![format!("{}/{}", base, decoded), decoded.clone()]
    };
    for c in cands {
        let n = normalize_zip_path(&c);
        if names.iter().any(|x| x == &n) {
            return Some(n);
        }
    }
    None
}

fn percent_decode(s: &str) -> String {
    let mut out = String::new();
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let (Some(h), Some(l)) = (hex(b[i + 1]), hex(b[i + 2])) {
                out.push((h << 4 | l) as char);
                i += 3;
                continue;
            }
        }
        out.push(b[i] as char);
        i += 1;
    }
    out
}

fn hex(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

struct Opf {
    manifest: HashMap<String, (String, String, String)>, // id -> (href, media, props)
    spine: Vec<(String, bool)>,                          // (idref, linear-no?)
    title: String,
    author: String,
}

fn parse_opf(xml: &[u8]) -> Opf {
    use quick_xml::events::Event;
    use quick_xml::reader::Reader;
    let mut r = Reader::from_reader(xml);
    r.config_mut().trim_text(true);
    let mut manifest = HashMap::new();
    let mut spine = Vec::new();
    let mut title = String::new();
    let mut author = String::new();
    let mut cur: Vec<u8> = Vec::new();
    let mut buf = Vec::new();
    loop {
        match r.read_event_into(&mut buf) {
            Ok(Event::Empty(e)) | Ok(Event::Start(e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).to_string();
                let lname = name.split(':').last().unwrap_or("").to_string();
                if lname == "item" {
                    let (mut id, mut href, mut media, mut props) = (String::new(), String::new(), String::new(), String::new());
                    for a in e.attributes().flatten() {
                        let k = String::from_utf8_lossy(a.key.as_ref()).to_string();
                        let v = a.unescape_value().unwrap_or_default().to_string();
                        match k.as_str() {
                            "id" => id = v,
                            "href" => href = v,
                            "media-type" => media = v,
                            "properties" => props = v,
                            _ => {}
                        }
                    }
                    if !id.is_empty() {
                        manifest.insert(id, (href, media, props));
                    }
                } else if lname == "itemref" {
                    let (mut idref, mut linear) = (String::new(), String::new());
                    for a in e.attributes().flatten() {
                        let k = String::from_utf8_lossy(a.key.as_ref()).to_string();
                        let v = a.unescape_value().unwrap_or_default().to_string();
                        if k == "idref" {
                            idref = v;
                        } else if k == "linear" {
                            linear = v;
                        }
                    }
                    if !idref.is_empty() {
                        spine.push((idref, linear == "no"));
                    }
                } else if lname == "title" || lname == "creator" {
                    cur = lname.into_bytes();
                }
            }
            Ok(Event::Text(e)) => {
                let t = e.unescape().unwrap_or_default().to_string();
                if cur == b"title" {
                    title.push_str(&t);
                } else if cur == b"creator" {
                    if !author.is_empty() {
                        author.push(' ');
                    }
                    author.push_str(&t);
                }
            }
            Ok(Event::End(e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).to_string();
                let lname = name.split(':').last().unwrap_or("");
                if lname == "title" || lname == "creator" {
                    cur.clear();
                }
            }
            Ok(Event::Eof) => break,
            _ => {}
        }
        buf.clear();
    }
    Opf { manifest, spine, title: title.trim().to_string(), author: author.trim().to_string() }
}

#[derive(Debug)]
struct RawToc {
    href: String,
    title: String,
    level: usize,
}

fn parse_ncx(xml: &[u8]) -> Vec<RawToc> {
    use quick_xml::events::Event;
    use quick_xml::reader::Reader;
    let mut r = Reader::from_reader(xml);
    r.config_mut().trim_text(true);
    let mut out = Vec::new();
    let mut stack: Vec<(String, String, usize)> = Vec::new(); // (href,title,depth)
    let mut in_text = false;
    let mut buf = Vec::new();
    loop {
        match r.read_event_into(&mut buf) {
            Ok(Event::Start(e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).to_string();
                let lname = name.split(':').last().unwrap_or("").to_string();
                if lname == "navPoint" {
                    stack.push((String::new(), String::new(), stack.len() + 1));
                } else if lname == "text" {
                    in_text = true;
                } else if lname == "content" {
                    for a in e.attributes().flatten() {
                        let k = String::from_utf8_lossy(a.key.as_ref()).to_string();
                        if k == "src" {
                            if let Some(top) = stack.last_mut() {
                                top.0 = a.unescape_value().unwrap_or_default().to_string();
                            }
                        }
                    }
                }
            }
            Ok(Event::Empty(e)) => {
                // <content src="..."/> 是自闭合,走 Empty 事件
                let name = String::from_utf8_lossy(e.name().as_ref()).to_string();
                let lname = name.split(':').last().unwrap_or("");
                if lname == "content" {
                    for a in e.attributes().flatten() {
                        let k = String::from_utf8_lossy(a.key.as_ref()).to_string();
                        if k == "src" {
                            if let Some(top) = stack.last_mut() {
                                top.0 = a.unescape_value().unwrap_or_default().to_string();
                            }
                        }
                    }
                }
            }
            Ok(Event::Text(e)) => {
                if in_text {
                    if let Some(top) = stack.last_mut() {
                        top.1.push_str(&e.unescape().unwrap_or_default());
                    }
                }
            }
            Ok(Event::End(e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).to_string();
                let lname = name.split(':').last().unwrap_or("").to_string();
                if lname == "text" {
                    in_text = false;
                } else if lname == "navPoint" {
                    if let Some((href, title, depth)) = stack.pop() {
                        out.push(RawToc { href, title: title.trim().to_string(), level: depth.clamp(1, 6) });
                    }
                }
            }
            Ok(Event::Eof) => break,
            _ => {}
        }
        buf.clear();
    }
    out
}

fn parse_nav_toc(html: &[u8]) -> Vec<RawToc> {
    // EPUB3 nav: <nav epub:type="toc"> 内 <ol><li><a href> 嵌套层级
    let s = String::from_utf8_lossy(html).to_string();
    let doc = Html::parse_document(&s);
    let nav_sel = Selector::parse("nav").unwrap();
    let mut nav_html: Option<String> = None;
    for nav in doc.select(&nav_sel) {
        let et = nav.value().attr("epub:type").unwrap_or("").to_string()
            + " " + nav.value().attr("type").unwrap_or("");
        if et.contains("toc") {
            nav_html = Some(nav.inner_html());
            break;
        }
    }
    // 兜底:第一个含 ol 的 nav
    if nav_html.is_none() {
        for nav in doc.select(&nav_sel) {
            let h = nav.inner_html();
            if h.contains("<ol") || h.contains("<ul") {
                nav_html = Some(h);
                break;
            }
        }
    }
    let inner = match nav_html {
        Some(h) => h,
        None => return Vec::new(),
    };
    // 用正则思路走 scraper:递归 ol>li
    let frag = Html::parse_fragment(&inner);
    let ol_sel = Selector::parse("ol,ul").unwrap();
    let a_sel = Selector::parse("a").unwrap();
    let mut out = Vec::new();
    // 只处理顶层 ol
    if let Some(top) = frag.select(&ol_sel).next() {
        walk_ol(&top, 1, &a_sel, &ol_sel, &mut out);
    }
    return out;

    fn walk_ol(
        ol: &scraper::ElementRef,
        level: usize,
        a_sel: &Selector,
        ol_sel: &Selector,
        out: &mut Vec<RawToc>,
    ) {
        // scraper 没有 :scope,直属 li 判定不可靠;基于字符串按顶层 <li> 切分
        // (嵌套 ol 整体保留),再递归,避免嵌套 li 被重复抓取。
        let html = ol.inner_html();
        for li_html in split_top_li(&html) {
            // 标题与 href 取第一个 <a>
            let f = Html::parse_fragment(&li_html);
            if let Some(a) = f.select(a_sel).next() {
                let href = a.value().attr("href").unwrap_or("").to_string();
                let title: String = a.text().collect::<Vec<_>>().join(" ").trim().to_string();
                if !href.is_empty() {
                    out.push(RawToc { href, title, level: level.clamp(1, 6) });
                }
                // 嵌套 ol
                if let Some(sub) = f.select(ol_sel).next() {
                    walk_ol(&sub, level + 1, a_sel, ol_sel, out);
                }
            }
        }
    }

    fn split_top_li(html: &str) -> Vec<String> {
        // 按顶层 <li> 切分,嵌套 <ol>/<ul> 内的 li 不切
        let mut res = Vec::new();
        let lower = html.to_lowercase();
        let mut i = 0;
        let n = html.len();
        while i < n {
            let li_pos = match lower[i..].find("<li") {
                Some(p) => i + p,
                None => break,
            };
            // 跳过嵌套 ol 内的 li:统计 li_pos 之前未闭合的 ol 深度 vs li 深度
            // 简化:从 li_pos 向后找配对 </li>,其间 ol 深度归零处结束
            let mut depth_ol = 0i32;
            let mut j = li_pos;
            // 找到 <li...> 结束 '>'
            let li_open_end = match html[j..].find('>') {
                Some(p) => j + p + 1,
                None => break,
            };
            j = li_open_end;
            let start = li_pos;
            let mut end: Option<usize> = None;
            while j < n {
                let rest = &lower[j..];
                if rest.starts_with("<ol") || rest.starts_with("<ul") {
                    depth_ol += 1;
                    j += 3;
                } else if rest.starts_with("</ol") || rest.starts_with("</ul") {
                    depth_ol -= 1;
                    if let Some(p) = html[j..].find('>') {
                        j += p + 1;
                    } else {
                        break;
                    }
                } else if rest.starts_with("</li") && depth_ol <= 0 {
                    if let Some(p) = html[j..].find('>') {
                        end = Some(j + p + 1);
                        j += p + 1;
                    }
                    break;
                } else if rest.starts_with("<li") && depth_ol <= 0 {
                    // 下一个同级 li 开始,上一个 li 未闭合(容错)
                    end = Some(j);
                    break;
                } else {
                    j += 1;
                }
            }
            let e = end.unwrap_or(n);
            res.push(html[start..e].to_string());
            i = e;
            if res.len() > 20000 {
                break;
            }
        }
        res
    }
}

fn extract_blocks(xhtml: &[u8]) -> (Vec<TextBlock>, Vec<(String, usize)>) {
    // 返回 (blocks, [(id, blockIdx)])
    let s = String::from_utf8_lossy(xhtml).to_string();
    let doc = Html::parse_document(&s);
    let sel_str = "p,h1,h2,h3,h4,h5,h6".to_string();
    let sel = Selector::parse(&sel_str).unwrap();
    let mut blocks = Vec::new();
    let mut ids = Vec::new();
    for el in doc.select(&sel) {
        let tag = el.value().name().to_lowercase();
        let level = match tag.as_str() {
            "h1" => 1,
            "h2" => 2,
            "h3" => 3,
            "h4" => 4,
            "h5" => 5,
            "h6" => 6,
            _ => 0,
        };
        let text: String = el.text().collect::<Vec<_>>().join(" ");
        let text = text.split_whitespace().collect::<Vec<_>>().join(" ").trim().to_string();
        if text.is_empty() {
            continue;
        }
        // 跳过纯数字页码之类极短无意义?保留,交给 speakable 过滤
        let idx = blocks.len();
        if let Some(id) = el.value().attr("id") {
            if !id.is_empty() {
                ids.push((id.to_string(), idx));
            }
        }
        blocks.push(TextBlock { text, level });
    }
    // 兜底:没有任何 p/h 标签(如纯 div 排版),按 div/section 文本切
    if blocks.is_empty() {
        let div_sel = Selector::parse("div,section,body").unwrap();
        for el in doc.select(&div_sel) {
            // 只取叶子 div(不含嵌套 div)
            let h = el.inner_html();
            if h.contains("<div") || h.contains("<p") {
                continue;
            }
            let text: String = el.text().collect::<Vec<_>>().join(" ");
            let text = text.split_whitespace().collect::<Vec<_>>().join(" ").trim().to_string();
            if text.chars().count() >= 2 {
                let idx = blocks.len();
                if let Some(id) = el.value().attr("id") {
                    if !id.is_empty() {
                        ids.push((id.to_string(), idx));
                    }
                }
                blocks.push(TextBlock { text, level: 0 });
                if blocks.len() > 20000 {
                    break;
                }
            }
        }
    }
    (blocks, ids)
}

fn is_speakable(t: &str) -> bool {
    let t = t.trim();
    if t.is_empty() {
        return false;
    }
    // 至少含一个字母/数字/CJK
    t.chars().any(|c| c.is_alphanumeric() || ('\u{4e00}'..='\u{9fff}').contains(&c))
}

fn open_epub_impl(path: &str) -> Result<BookData, String> {
    let f = std::fs::File::open(path).map_err(|e| format!("打不开文件: {}", e))?;
    let mut zip = zip::ZipArchive::new(f).map_err(|e| format!("不是合法 EPUB: {}", e))?;
    let names: Vec<String> = zip.file_names().map(|s| s.to_string()).collect();

    // container.xml
    let container = names
        .iter()
        .find(|n| n.to_lowercase().ends_with("container.xml"))
        .cloned()
        .unwrap_or("META-INF/container.xml".to_string());
    let container_xml = zip_read(&mut zip, &container).ok_or("缺少 container.xml")?;
    let container_s = String::from_utf8_lossy(&container_xml).to_string();
    // 找 rootfile full-path
    let opf_path = {
        let mut p = "OEBPS/content.opf".to_string();
        if let Some(i) = container_s.find("full-path") {
            let rest = &container_s[i..];
            if let Some(q1) = rest.find('"') {
                if let Some(q2) = rest[q1 + 1..].find('"') {
                    p = rest[q1 + 1..q1 + 1 + q2].to_string();
                }
            }
        }
        // 验证存在,否则在 manifest 里找 .opf
        if !names.iter().any(|n| n == &p) {
            if let Some(n) = names.iter().find(|n| n.to_lowercase().ends_with(".opf")) {
                p = n.clone();
            }
        }
        p
    };
    let opf_bytes = zip_read(&mut zip, &opf_path).ok_or("缺少 OPF")?;
    let opf = parse_opf(&opf_bytes);
    let base = opf_path.rfind('/').map(|i| opf_path[..i].to_string()).unwrap_or_default();

    // spine → 章节
    let mut chapters: Vec<Chapter> = Vec::new();
    let mut id_index: HashMap<(usize, String), usize> = HashMap::new(); // (spineIdx, id)->blockIdx
    let mut spine_hrefs: Vec<String> = Vec::new();
    let mut chapter_at_spine: Vec<i64> = Vec::new();
    for (si, (idref, linear_no)) in opf.spine.iter().enumerate() {
        let (href, _media, _props) = match opf.manifest.get(idref) {
            Some(v) => v.clone(),
            None => {
                chapter_at_spine.push(-1);
                spine_hrefs.push(String::new());
                continue;
            }
        };
        let entry = match resolve_entry(&names, &base, &href) {
            Some(e) => e,
            None => {
                chapter_at_spine.push(-1);
                spine_hrefs.push(String::new());
                continue;
            }
        };
        spine_hrefs.push(entry.clone());
        if *linear_no {
            chapter_at_spine.push(-1);
            continue;
        }
        let bytes = match zip_read(&mut zip, &entry) {
            Some(b) => b,
            None => {
                chapter_at_spine.push(-1);
                continue;
            }
        };
        // 只要文本类内容才成章(跳过图片页)
        let (blocks, ids) = extract_blocks(&bytes);
        let ci = chapters.len() as i64;
        chapter_at_spine.push(ci);
        for (id, bi) in ids {
            id_index.insert((si, id), bi);
        }
        let title = blocks
            .iter()
            .find(|b| (1..=6).contains(&b.level))
            .map(|b| b.text.clone())
            .unwrap_or_else(|| {
                Path::new(&entry)
                    .file_stem()
                    .map(|s| s.to_string_lossy().to_string())
                    .unwrap_or_else(|| format!("第{}章", ci + 1))
            });
        chapters.push(Chapter { title, blocks });
        let _ = si;
    }

    // 目录:NCX > nav > h1h2 > 逐章
    let mut toc: Vec<TocEntry> = Vec::new();
    // ncx
    let ncx_entry = opf
        .manifest
        .values()
        .find(|(_, m, _)| m == "application/x-dtbncx+xml")
        .map(|(h, _, _)| h.clone())
        .or_else(|| {
            opf.manifest
                .values()
                .find(|(h, _, _)| h.to_lowercase().ends_with(".ncx"))
                .map(|(h, _, _)| h.clone())
        })
        .and_then(|h| resolve_entry(&names, &base, &h));
    let mut raw: Vec<RawToc> = Vec::new();
    if let Some(ncx) = ncx_entry {
        if let Some(b) = zip_read(&mut zip, &ncx) {
            raw = parse_ncx(&b);
        }
    }
    if raw.is_empty() {
        // nav
        let nav_href = opf
            .manifest
            .values()
            .find(|(_, _, p)| p.contains("nav"))
            .map(|(h, _, _)| h.clone())
            .and_then(|h| resolve_entry(&names, &base, &h));
        if let Some(nav) = nav_href {
            if let Some(b) = zip_read(&mut zip, &nav) {
                raw = parse_nav_toc(&b);
            }
        }
    }
    if !raw.is_empty() {
        let spine_index_of: HashMap<String, usize> =
            spine_hrefs.iter().enumerate().map(|(i, h)| (h.clone(), i)).collect();
        for r in raw {
            let file = r.href.split('#').next().unwrap_or("").trim();
            let frag = if r.href.contains('#') {
                percent_decode(&r.href.split('#').skip(1).collect::<Vec<_>>().join("#"))
            } else {
                String::new()
            };
            let entry = match resolve_entry(&names, &base, file) {
                Some(e) => e,
                None => continue,
            };
            let si = match spine_index_of.get(&entry) {
                Some(v) => *v,
                None => continue,
            };
            let ci = match chapter_at_spine.get(si) {
                Some(v) if *v >= 0 => *v as usize,
                _ => continue,
            };
            let bc = chapters.get(ci).map(|c| c.blocks.len()).unwrap_or(0);
            if bc == 0 {
                continue;
            }
            let bi = if !frag.is_empty() {
                id_index.get(&(si, frag)).copied().unwrap_or(0).min(bc - 1)
            } else {
                0
            };
            let title = if r.title.trim().is_empty() {
                chapters.get(ci).map(|c| c.title.clone()).unwrap_or_default()
            } else {
                r.title
            };
            toc.push(TocEntry { index: toc.len(), title, level: r.level.clamp(1, 6), chapter: ci, block: bi });
        }
    }
    if toc.is_empty() {
        // 回退:h1/h2;再没有逐章一条(跳过空章节,避免空章把起止位置撑到全书)
        for (ci, c) in chapters.iter().enumerate() {
            if c.blocks.is_empty() {
                continue;
            }
            let mut added = false;
            for (bi, b) in c.blocks.iter().enumerate() {
                if (1..=2).contains(&b.level) && !b.text.trim().is_empty() {
                    toc.push(TocEntry {
                        index: toc.len(),
                        title: b.text.clone(),
                        level: b.level,
                        chapter: ci,
                        block: bi,
                    });
                    added = true;
                }
            }
            if !added {
                toc.push(TocEntry {
                    index: toc.len(),
                    title: c.title.clone(),
                    level: 1,
                    chapter: ci,
                    block: 0,
                });
            }
        }
    }
    // 修正 index
    for (i, e) in toc.iter_mut().enumerate() {
        e.index = i;
    }
    let title = if opf.title.is_empty() {
        Path::new(path)
            .file_stem()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| "未知书名".to_string())
    } else {
        opf.title
    };
    Ok(BookData { title, author: opf.author, chapters, toc, path: path.to_string() })
}

// segment: toc[i] 覆盖 [start, 下一个有效start),与 aurora segmentEbook 同口径。
// 健壮性:空章节/乱序 NCX 导致的无效起止会被跳过(只取 >s 的下一个有效起点),
// 空条目返回空正文(调用方跳过,不产只有标题的空文件)。
fn segment_range(book: &BookData, toc_idx: usize) -> (String, Vec<String>) {
    // flat
    let mut flat: Vec<(usize, usize, String)> = Vec::new();
    for (ci, c) in book.chapters.iter().enumerate() {
        for (bi, b) in c.blocks.iter().enumerate() {
            flat.push((ci, bi, b.text.clone()));
        }
    }
    let title = book
        .toc
        .get(toc_idx)
        .map(|e| e.title.trim().to_string())
        .unwrap_or_default();
    let title = if title.is_empty() { "Section".to_string() } else { title };
    if flat.is_empty() {
        return (title, vec![]);
    }
    let toc_pos = |ti: usize| -> Option<usize> {
        let e = &book.toc[ti];
        if let Some(p) = flat.iter().position(|(ci, bi, _)| *ci == e.chapter && *bi >= e.block) {
            return Some(p);
        }
        flat.iter().position(|(ci, _, _)| *ci == e.chapter)
    };
    let s = match toc_pos(toc_idx) {
        Some(p) => p.min(flat.len()),
        None => return (title, vec![]),
    };
    // 终点:后面第一个落在 (s, flat.len()] 的有效起点,否则全书末尾
    let mut e = flat.len();
    for tj in (toc_idx + 1)..book.toc.len() {
        if let Some(p) = toc_pos(tj) {
            if p > s {
                e = p.min(flat.len());
                break;
            }
        }
    }
    let mut body: Vec<String> = Vec::new();
    for i in s..e {
        if let Some((_, _, t)) = flat.get(i) {
            let t = t.trim().to_string();
            if !t.is_empty() && is_speakable(&t) {
                body.push(t);
            }
        }
    }
    // 段首播报标题(除非第一块本来就是标题)
    let first = flat.get(s).map(|(_, _, t)| t.trim().to_string()).unwrap_or_default();
    let mut texts = Vec::new();
    if !first.eq_ignore_ascii_case(title.trim()) {
        texts.push(title.clone());
    }
    texts.extend(body.clone());
    (title, texts)
}

// ---------- mstts 池 ----------

// ---------- mstts 与语音包定位(包内自带优先零配置,用户已装完整版则沿用) ----------

/// 包内资源查找:bundler 会把 `resources/xxx` 原样放到
/// `Resources/resources/xxx`,两种布局都试(先短后长)。
/// dev 下 target/.../Resources 不存在,返回 None。
fn resource_sub(rel: &str) -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let d = exe.parent()?;
    for base in ["../Resources", "../Resources/resources"] {
        let p = d.join(base).join(rel);
        if p.exists() {
            return Some(p);
        }
    }
    None
}

/// 打包进 Resources 的文件可能丢掉可执行位,运行前补上。
fn ensure_exec(p: &Path) {
    if let Ok(md) = std::fs::metadata(p) {
        let mut perm = md.permissions();
        let mode = perm.mode();
        if mode & 0o111 == 0 {
            perm.set_mode(mode | 0o755);
            let _ = std::fs::set_permissions(p, perm);
        }
    }
}

/// 目录下是否有任一语音(子目录含 Tokens.xml)。
fn has_any_voice(dir: &Path) -> bool {
    std::fs::read_dir(dir)
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .any(|e| e.path().join("Tokens.xml").is_file())
        })
        .unwrap_or(false)
}

fn find_mstts() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("MSTTS_BIN") {
        let pb = PathBuf::from(&p);
        if pb.is_file() {
            return Some(pb);
        }
    }
    // 包内自带(分发零依赖)
    if let Some(m) = resource_sub("mstts/mstts") {
        if m.is_file() {
            ensure_exec(&m);
            return Some(m);
        }
    }
    // 开发期:内嵌源码构建产物(<proj>/mstts/build/mstts,见 scripts/build-mstts.sh)
    if let Ok(exe) = std::env::current_exe() {
        if let Some(d) = exe.parent() {
            // cargo dev/debug: .../src-tauri/target/debug/<exe>
            let dev_m = d.join("../../../mstts/build/mstts");
            if dev_m.is_file() {
                return Some(dev_m);
            }
        }
    }
    // PATH
    if let Ok(out) = std::process::Command::new("which").arg("mstts").output() {
        let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if !s.is_empty() && Path::new(&s).is_file() {
            return Some(PathBuf::from(s));
        }
    }
    None
}

fn default_voices_dir() -> String {
    if let Ok(v) = std::env::var("MSTTS_VOICES") {
        if !v.is_empty() {
            return v;
        }
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".to_string());
    let installed = format!("{}/Library/Application Support/mstts/voices", home);
    // 用户已装完整语音则沿用(9 语音全可用);否则用包内自带的晓晓+Jenny(零配置)。
    if has_any_voice(Path::new(&installed)) {
        return installed;
    }
    // 开发期:仓库里的 resources/voices(随包提交,给没有装语音包的 fresh clone 用)
    if let Ok(exe) = std::env::current_exe() {
        if let Some(d) = exe.parent() {
            let dev_v = d.join("../../resources/voices");
            if has_any_voice(&dev_v) {
                return dev_v.to_string_lossy().to_string();
            }
        }
    }
    if let Some(v) = resource_sub("voices") {
        if has_any_voice(&v) {
            return v.to_string_lossy().to_string();
        }
    }
    installed
}

fn builtin_voices() -> Vec<VoiceInfo> {
    vec![
        ("zh-CN-XiaoxiaoNeural", "晓晓", "zh-CN", "Female"),
        ("zh-CN-YunxiNeural", "云希", "zh-CN", "Male"),
        ("zh-TW-YunJheNeural", "云哲", "zh-TW", "Female"),
        ("en-US-AriaNeural", "Aria", "en-US", "Female"),
        ("en-US-JennyNeural", "Jenny", "en-US", "Female"),
        ("en-US-GuyNeural", "Guy", "en-US", "Male"),
        ("en-GB-RyanNeural", "Ryan", "en-GB", "Male"),
        ("en-IN-NeerjaNeural", "Neerja", "en-IN", "Female"),
        ("en-IN-PrabhatNeural", "Prabhat", "en-IN", "Male"),
    ]
    .into_iter()
    .map(|(id, name, locale, gender)| VoiceInfo {
        id: id.into(),
        name: name.into(),
        locale: locale.into(),
        gender: gender.into(),
        installed: true,
    })
    .collect()
}

fn list_voices_impl() -> Vec<VoiceInfo> {
    let mstts = match find_mstts() {
        Some(p) => p,
        None => return builtin_voices(),
    };
    let out = std::process::Command::new(&mstts)
        .arg("list")
        .arg("--json")
        .env("MSTTS_VOICES", default_voices_dir())
        .output();
    let out = match out {
        Ok(o) => o,
        Err(_) => return builtin_voices(),
    };
    let s = String::from_utf8_lossy(&out.stdout).to_string();
    // {"voices":[{"id","short","locale","gender","installed"}]}
    let v: serde_json::Value = match serde_json::from_str(&s) {
        Ok(v) => v,
        Err(_) => return builtin_voices(),
    };
    let mut res = Vec::new();
    if let Some(arr) = v.get("voices").and_then(|x| x.as_array()) {
        for it in arr {
            res.push(VoiceInfo {
                id: it.get("id").and_then(|x| x.as_str()).unwrap_or("").to_string(),
                name: it.get("short").and_then(|x| x.as_str()).unwrap_or("").to_string(),
                locale: it.get("locale").and_then(|x| x.as_str()).unwrap_or("").to_string(),
                gender: it.get("gender").and_then(|x| x.as_str()).unwrap_or("").to_string(),
                installed: it.get("installed").and_then(|x| x.as_bool()).unwrap_or(false),
            });
        }
    }
    if res.is_empty() {
        return builtin_voices();
    }
    res
}

struct TtsPool {
    ports: Vec<u16>,
    _children: Vec<std::process::Child>,
    client: reqwest::Client,
    next: AtomicUsize,
}

impl TtsPool {
    async fn start(n: usize) -> Result<Self, String> {
        let mstts = find_mstts().ok_or("找不到 mstts 可执行文件(请先在 mstts-macos 下 make)")?;
        let voices = default_voices_dir();
        let client = reqwest::Client::builder()
            .timeout(Duration::from_secs(120))
            .build()
            .map_err(|e| e.to_string())?;
        // 选空闲端口段
        let base = 18871 + (std::process::id() % 500) as u16;
        let mut ports = Vec::new();
        let mut children = Vec::new();
        for i in 0..n {
            let port = base + (i as u16) * 7;
            let mut cmd = std::process::Command::new(&mstts);
            cmd.arg("serve")
                .arg("--port")
                .arg(port.to_string())
                .env("MSTTS_VOICES", &voices)
                .stdout(Stdio::null())
                .stderr(Stdio::null());
            let child = cmd.spawn().map_err(|e| format!("启动 mstts serve 失败: {}", e))?;
            children.push(child);
            ports.push(port);
        }
        // 等健康
        for _ in 0..100 {
            let mut ok = 0;
            for p in &ports {
                if client.get(format!("http://127.0.0.1:{}/health", p)).send().await.is_ok() {
                    ok += 1;
                }
            }
            if ok == ports.len() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        // 最终检查
        for p in &ports {
            if client.get(format!("http://127.0.0.1:{}/health", p)).send().await.is_err() {
                for c in children {
                    let _ = std::process::Command::new("kill").arg(c.id().to_string()).output();
                }
                return Err(format!("mstts serve 未就绪(端口 {})", p));
            }
        }
        Ok(Self { ports, _children: children, client, next: AtomicUsize::new(0) })
    }

    async fn synth_pcm(&self, text: &str, voice: &str, rate: f32, pitch: f32) -> Result<Vec<u8>, String> {
        // 轮询选 worker,失败换一个重试
        let mut last = String::new();
        for _ in 0..self.ports.len().max(1) {
            let i = self.next.fetch_add(1, Ordering::Relaxed) % self.ports.len().max(1);
            let port = self.ports[i];
            let body = serde_json::json!({
                "input": text, "voice": voice,
                "speed": rate, "pitch": pitch,
                "response_format": "pcm"
            });
            match self
                .client
                .post(format!("http://127.0.0.1:{}/v1/audio/speech", port))
                .json(&body)
                .send()
                .await
            {
                Ok(r) => {
                    if !r.status().is_success() {
                        let t = r.text().await.unwrap_or_default();
                        last = format!("TTS {}: {}", port, t.chars().take(200).collect::<String>());
                        continue;
                    }
                    match r.bytes().await {
                        Ok(b) => {
                            if b.is_empty() {
                                last = "空音频(语言与语音不匹配?)".to_string();
                                continue;
                            }
                            return Ok(b.to_vec());
                        }
                        Err(e) => {
                            last = e.to_string();
                        }
                    }
                }
                Err(e) => {
                    last = e.to_string();
                }
            }
        }
        Err(last)
    }
}

impl Drop for TtsPool {
    fn drop(&mut self) {
        for c in self._children.iter_mut() {
            let _ = c.kill();
        }
    }
}

// 按<p>维度切块:每块一段,超长块按句再切(≤max_chars)
fn chunk_paragraphs(texts: &[String], max_chars: usize) -> Vec<String> {
    let mut out = Vec::new();
    for t in texts {
        let t = t.trim();
        if t.is_empty() || !is_speakable(t) {
            continue;
        }
        if t.chars().count() <= max_chars {
            out.push(t.to_string());
            continue;
        }
        // 按句切
        let mut cur = String::new();
        let mut cur_n = 0usize;
        let mut sent = String::new();
        let mut sent_n = 0usize;
        let push_sent = |sent: &mut String, sent_n: &mut usize, cur: &mut String, cur_n: &mut usize, out: &mut Vec<String>| {
            if sent.trim().is_empty() {
                return;
            }
            if *cur_n + *sent_n + 1 > max_chars && !cur.is_empty() {
                out.push(cur.trim().to_string());
                cur.clear();
                *cur_n = 0;
            }
            if !cur.is_empty() {
                cur.push('\n');
            }
            cur.push_str(sent.trim());
            *cur_n += *sent_n;
            sent.clear();
            *sent_n = 0;
        };
        for c in t.chars() {
            sent.push(c);
            sent_n += 1;
            if "。！？；!?\n…".contains(c) {
                push_sent(&mut sent, &mut sent_n, &mut cur, &mut cur_n, &mut out);
            }
            // 单句超长硬切
            if sent_n >= max_chars {
                push_sent(&mut sent, &mut sent_n, &mut cur, &mut cur_n, &mut out);
            }
        }
        if !sent.trim().is_empty() {
            push_sent(&mut sent, &mut sent_n, &mut cur, &mut cur_n, &mut out);
        }
        if !cur.trim().is_empty() {
            out.push(cur.trim().to_string());
        }
    }
    out
}

fn sanitize_title(t: &str) -> String {
    let mut s = t.trim().to_string();
    for c in ['/', '\\', ':', '*', '?', '"', '<', '>', '|'] {
        s = s.replace(c, "_");
    }
    s = s.chars().filter(|c| !c.is_control()).collect();
    s = s.split_whitespace().collect::<Vec<_>>().join(" ");
    s = s.trim().trim_matches('.').trim().to_string();
    if s.is_empty() {
        s = "Section".to_string();
    }
    // 限长80字符(按 char)
    if s.chars().count() > 80 {
        s = s.chars().take(80).collect::<String>().trim_end().to_string();
    }
    s
}

fn write_wav(path: &Path, pcm: &[u8], sample_rate: u32) -> std::io::Result<()> {
    // 16bit mono
    let mut f = std::fs::File::create(path)?;
    let data_len = pcm.len() as u32;
    let file_len = 36 + data_len;
    f.write_all(b"RIFF")?;
    f.write_all(&file_len.to_le_bytes())?;
    f.write_all(b"WAVEfmt ")?;
    f.write_all(&16u32.to_le_bytes())?;
    f.write_all(&1u16.to_le_bytes())?;
    f.write_all(&1u16.to_le_bytes())?;
    f.write_all(&sample_rate.to_le_bytes())?;
    f.write_all(&(sample_rate * 2).to_le_bytes())?;
    f.write_all(&2u16.to_le_bytes())?;
    f.write_all(&16u16.to_le_bytes())?;
    f.write_all(b"data")?;
    f.write_all(&data_len.to_le_bytes())?;
    f.write_all(pcm)?;
    Ok(())
}

// ---------- Tauri 命令 ----------

#[tauri::command]
fn open_epub(path: String) -> Result<BookInfo, String> {
    let book = open_epub_impl(&path)?;
    let id = format!("b{}", books().lock().unwrap().len() + 1);
    let info = BookInfo {
        id: id.clone(),
        title: book.title.clone(),
        author: book.author.clone(),
        toc: book.toc.clone(),
        total: book.toc.len(),
        dir: Path::new(&path)
            .parent()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default(),
    };
    books().lock().unwrap().insert(id, book);
    Ok(info)
}

#[tauri::command]
fn get_segment(book_id: String, toc_index: usize) -> Result<SegmentPreview, String> {
    let map = books().lock().unwrap();
    let book = map.get(&book_id).ok_or("书籍已失效,请重新打开")?;
    if toc_index >= book.toc.len() {
        return Err("章节越界".into());
    }
    let (title, texts) = segment_range(book, toc_index);
    // 预览去掉段首标题播报(正文即段落)
    let mut body_only = texts.clone();
    if !body_only.is_empty() && body_only[0].trim() == title.trim() {
        body_only.remove(0);
    }
    let chars: usize = body_only.iter().map(|s| s.chars().count()).sum();
    Ok(SegmentPreview { title, paragraphs: body_only, chars })
}

#[tauri::command]
fn list_voices() -> Vec<VoiceInfo> {
    list_voices_impl()
}

#[tauri::command]
async fn preview_speech(
    text: String,
    voice: String,
    rate: f32,
    pitch: f32,
) -> Result<String, String> {
    // 单 worker 合成试听,返回 dataURL(wav base64)
    let pool = TtsPool::start(1).await?;
    let t: String = text.chars().take(600).collect();
    let pcm = pool.synth_pcm(&t, &voice, rate, pitch).await?;
    let tmp = std::env::temp_dir().join(format!("epub_preview_{}.wav", std::process::id()));
    write_wav(&tmp, &pcm, 24000).map_err(|e| e.to_string())?;
    let wav = std::fs::read(&tmp).map_err(|e| e.to_string())?;
    let _ = std::fs::remove_file(&tmp);
    Ok(format!("data:audio/wav;base64,{}", base64::Engine::encode(&base64::engine::general_purpose::STANDARD, &wav)))
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ExportParams {
    book_id: String,
    indices: Vec<usize>,
    out_dir: String,
    voice: String,
    rate: f32,
    pitch: f32,
    threads: usize,
    bitrate: usize, // kbps
}

#[tauri::command]
async fn start_export(app: AppHandle, params: ExportParams) -> Result<String, String> {
    cancelled_flag().store(false, Ordering::SeqCst);
    let map = books().lock().unwrap();
    let book = map.get(&params.book_id).ok_or("书籍已失效,请重新打开")?;
    let title = book.title.clone();
    let book_path = book.path.clone();
    // 快照 segments(标题+正文)
    let mut segs: Vec<(usize, String, Vec<String>)> = Vec::new();
    let mut idxs = params.indices.clone();
    idxs.sort_unstable();
    idxs.dedup();
    for ti in idxs {
        if ti >= book.toc.len() {
            continue;
        }
        let (t, texts) = segment_range(book, ti);
        let chunks = chunk_paragraphs(&texts, 450);
        if chunks.is_empty() {
            continue;
        }
        segs.push((ti, t, chunks));
    }
    drop(map);
    if segs.is_empty() {
        return Err("没有可转换的章节(空内容已跳过)".into());
    }
    let out_root = PathBuf::from(&params.out_dir);
    if !out_root.is_dir() {
        std::fs::create_dir_all(&out_root).map_err(|e| format!("输出目录不可用: {}", e))?;
    }
    // 输出:所选目录下新建与 epub 文件名(basename,去扩展名)同名的文件夹
    let stem = Path::new(&book_path)
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| title.clone());
    let folder = sanitize_title(&stem);
    let out_dir = out_root.join(&folder);
    std::fs::create_dir_all(&out_dir).map_err(|e| format!("无法创建输出子目录: {}", e))?;

    let threads = params.threads.clamp(1, 32);
    let voice = params.voice.clone();
    let rate = params.rate.clamp(0.5, 2.0);
    let pitch = params.pitch.clamp(0.5, 2.0);
    let bitrate = params.bitrate.clamp(24, 256);
    let total = segs.len();

    // 后台跑,事件推送进度(移动所有权进 task,segs/params 不借用)
    let app2 = app.clone();
    tokio::spawn(async move {
        let run = async {
            let pool = Arc::new(TtsPool::start(threads).await?);
            let sem = Arc::new(tokio::sync::Semaphore::new(threads));
            let mut done = 0usize;
            let mut skipped = 0usize;
            let mut failed: Vec<String> = Vec::new();
            let mut ok = 0usize;
            for (ord, (ti, seg_title, chunks)) in segs.iter().enumerate() {
                let ti = *ti;
                let seg_title = seg_title.clone();
                let chunks = chunks.clone();
                if cancelled_flag().load(Ordering::SeqCst) {
                    break;
                }
                // 已存在则直接跳过(不覆盖);编号按所选顺序固定,重跑结果稳定
                let fname = format!("{:04}-{}.m4a", ord + 1, sanitize_title(&seg_title));
                if out_dir.join(&fname).is_file() {
                    skipped += 1;
                    let _ = app2.emit(
                        "export-progress",
                        serde_json::json!({
                            "toc": ti, "doneChunks": chunks.len(), "totalChunks": chunks.len(),
                            "phase": "skipped", "done": done, "total": total, "title": seg_title,
                        }),
                    );
                    continue;
                }
                let n = chunks.len();
                let synth_done = Arc::new(AtomicUsize::new(0));
                // 并行合成各 <p> 块(取消时 select 直接中断,不等在途请求)
                let mut tasks = Vec::new();
                for (j, ch) in chunks.iter().enumerate() {
                    let sem = sem.clone();
                    let pool = pool.clone();
                    let text = ch.clone();
                    let voice = voice.clone();
                    let sd = synth_done.clone();
                    let app3 = app2.clone();
                    let seg_title_c = seg_title.clone();
                    tasks.push(tokio::spawn(async move {
                        let _p = sem.acquire().await.unwrap();
                        // 取消时不等服务端返回,直接记错并退出
                        let r = tokio::select! {
                            r = pool.synth_pcm(&text, &voice, rate, pitch) => r,
                            _ = cancel_notify().notified() => Err("已取消".to_string()),
                        };
                        let d = sd.fetch_add(1, Ordering::SeqCst) + 1;
                        let _ = app3.emit(
                            "export-progress",
                            serde_json::json!({
                                "toc": ti, "doneChunks": d, "totalChunks": n,
                                "phase": "synth", "done": done, "total": total,
                                "title": seg_title_c,
                            }),
                        );
                        (j, r)
                    }));
                }
                let mut parts: Vec<Option<Vec<u8>>> = (0..n).map(|_| None).collect();
                let mut seg_err: Option<String> = None;
                // 逐个等结果;取消到达则 abort 剩余全部任务,不再傻等
                let mut it = tasks.into_iter();
                let mut cancelled_now = false;
                while let Some(mut h) = it.next() {
                    let r = tokio::select! {
                        r = &mut h => Some(r),
                        _ = cancel_notify().notified() => {
                            h.abort();
                            None
                        }
                    };
                    match r {
                        Some(Ok((j, Ok(pcm)))) => parts[j] = Some(pcm),
                        Some(Ok((_, Err(e)))) => {
                            if seg_err.is_none() {
                                seg_err = Some(e);
                            }
                        }
                        Some(Err(e)) => {
                            if seg_err.is_none() {
                                seg_err = Some(e.to_string());
                            }
                        }
                        None => {
                            cancelled_now = true;
                        }
                    }
                    if cancelled_now || cancelled_flag().load(Ordering::SeqCst) {
                        cancelled_now = true;
                        break;
                    }
                }
                if cancelled_now {
                    // 全部 abort,落盘前直接收工
                    for h in it {
                        h.abort();
                    }
                    cancelled_flag().store(true, Ordering::SeqCst);
                    break;
                }
                if cancelled_flag().load(Ordering::SeqCst) {
                    break;
                }
                if let Some(e) = seg_err {
                    // 只要有一块失败,整段记失败(与 aurora 同策略:首错上报)
                    failed.push(format!("{}: {}", seg_title, e.chars().take(120).collect::<String>()));
                    let _ = app2.emit(
                        "export-progress",
                        serde_json::json!({
                            "toc": ti, "doneChunks": n, "totalChunks": n,
                            "phase": "failed", "done": done, "total": total, "title": seg_title,
                        }),
                    );
                    continue;
                }
                // 按序拼接
                let mut pcm_all: Vec<u8> = Vec::new();
                for p in parts.into_iter().flatten() {
                    pcm_all.extend_from_slice(&p);
                }
                if pcm_all.is_empty() {
                    failed.push(format!("{}: 无可用音频", seg_title));
                    continue;
                }
                let _ = app2.emit(
                    "export-progress",
                    serde_json::json!({
                        "toc": ti, "doneChunks": n, "totalChunks": n,
                        "phase": "encode", "done": done, "total": total, "title": seg_title,
                    }),
                );
                // 写 wav → afconvert → m4a(保持24k单声道,不重采样)
                // fname 在段首已算好(跳过检查用),此处复用,保证编号稳定
                let tmp_wav = std::env::temp_dir().join(format!("epub_seg_{}_{}.wav", std::process::id(), ti));
                let tmp_m4a = std::env::temp_dir().join(format!("epub_seg_{}_{}.m4a", std::process::id(), ti));
                if let Err(e) = write_wav(&tmp_wav, &pcm_all, 24000) {
                    failed.push(format!("{}: 写WAV失败 {}", seg_title, e));
                    continue;
                }
                let br = format!("{}", bitrate * 1000);
                let mut child = match tokio::process::Command::new("afconvert")
                    .arg("-f")
                    .arg("m4af")
                    .arg("-d")
                    .arg("aac")
                    .arg("-b")
                    .arg(&br)
                    .arg("-q")
                    .arg("127")
                    .arg(&tmp_wav)
                    .arg(&tmp_m4a)
                    .stdout(Stdio::piped())
                    .stderr(Stdio::piped())
                    .spawn()
                {
                    Ok(c) => c,
                    Err(e) => {
                        failed.push(format!("{}: afconvert不可用 {}", seg_title, e));
                        let _ = std::fs::remove_file(&tmp_wav);
                        continue;
                    }
                };
                // 取消时杀掉编码子进程,不傻等
                let status = tokio::select! {
                    r = child.wait() => Some(r),
                    _ = cancel_notify().notified() => {
                        let _ = child.start_kill();
                        None
                    }
                };
                let _ = std::fs::remove_file(&tmp_wav);
                let status = match status {
                    Some(Ok(s)) => s,
                    Some(Err(e)) => {
                        failed.push(format!("{}: 编码启动失败 {}", seg_title, e));
                        let _ = std::fs::remove_file(&tmp_m4a);
                        continue;
                    }
                    None => {
                        let _ = std::fs::remove_file(&tmp_m4a);
                        cancelled_flag().store(true, Ordering::SeqCst);
                        break;
                    }
                };
                // 读 stderr 用于报错信息(afconvert 输出很小,不会堵管道)
                let mut err = String::new();
                if let Some(mut stderr) = child.stderr.take() {
                    use tokio::io::AsyncReadExt as _;
                    let mut buf = Vec::new();
                    let _ = stderr.read_to_end(&mut buf).await;
                    err = String::from_utf8_lossy(&buf).chars().take(200).collect::<String>();
                }
                let conv_ok = status.success() && tmp_m4a.is_file();
                if conv_ok {
                    // 原生写标签(只改 box,不重编码);标签失败不算失败:直拷无标签音频照样可用
                    let dest = out_dir.join(&fname);
                    let trk = (ord + 1).min(65535) as u16;
                    let tot = total.min(65535) as u16;
                    let _ = tag_m4a(&tmp_m4a, &seg_title, &title, "epub", trk, tot);
                    let _ = std::fs::copy(&tmp_m4a, &dest);
                    let _ = std::fs::remove_file(&tmp_m4a);
                    if !dest.is_file() || dest.metadata().map(|m| m.len()).unwrap_or(0) == 0 {
                        failed.push(format!("{}: 无法写入输出文件", seg_title));
                        continue;
                    }
                    ok += 1;
                    done += 1;
                    let _ = app2.emit(
                        "export-progress",
                        serde_json::json!({
                            "toc": ti, "doneChunks": n, "totalChunks": n,
                            "phase": "done", "done": done, "total": total,
                            "title": seg_title, "file": fname,
                        }),
                    );
                } else {
                    failed.push(format!("{}: 编码失败 {}", seg_title, err));
                }
            }
            let cancelled = cancelled_flag().load(Ordering::SeqCst);
            Ok::<(usize, usize, Vec<String>, bool), String>((ok, skipped, failed, cancelled))
        };
        match run.await {
            Ok((ok, skipped, failed, cancelled)) => {
                let _ = app2.emit(
                    "export-done",
                    serde_json::json!({
                        "done": ok, "skipped": skipped, "failed": failed,
                        "cancelled": cancelled, "folder": folder.clone(),
                    }),
                );
            }
            Err(e) => {
                let _ = app2.emit(
                    "export-done",
                    serde_json::json!({ "done": 0, "skipped": 0, "failed": [e], "cancelled": false, "folder": "" }),
                );
            }
        }
    });
    Ok(format!("已开始转换,共{}段", total))
}

// ---------- M4A 标签:原生 MP4 box 读写(只改盒子,不碰音频,替代 ffmpeg) ----------

type BoxId = [u8; 4];

/// 读 box 头,返回 (总长, header长)。支持 size==1(largesize) 与 size==0(延至父末尾)。
fn mp4_box_size(buf: &[u8], p: usize, end: usize) -> Option<(u64, usize)> {
    if p.checked_add(8)? > end || end > buf.len() {
        return None;
    }
    let s32 = u32::from_be_bytes([buf[p], buf[p + 1], buf[p + 2], buf[p + 3]]);
    if s32 == 1 {
        if p + 16 > end {
            return None;
        }
        let s = u64::from_be_bytes(buf[p + 8..p + 16].try_into().ok()?);
        if s < 16 {
            return None;
        }
        Some((s, 16))
    } else if s32 == 0 {
        Some(((end - p) as u64, 8))
    } else {
        if (s32 as u64) < 8 {
            return None;
        }
        Some((s32 as u64, 8))
    }
}

/// 在 [start,end) 内找类型为 typ 的直接子 box,返回 (起点,终点)。
fn mp4_find(buf: &[u8], start: usize, end: usize, typ: &BoxId) -> Option<(usize, usize)> {
    let mut p = start;
    while p + 8 <= end {
        let (sz, _) = mp4_box_size(buf, p, end)?;
        if sz == 0 {
            break;
        }
        if &buf[p + 4..p + 8] == typ {
            let e = (p as u64 + sz) as usize;
            if e > end {
                return None;
            }
            return Some((p, e));
        }
        p = (p as u64 + sz) as usize;
    }
    None
}

/// 列出 [start,end) 内所有直接子 box:(类型, 起点, 终点)。
fn mp4_children(buf: &[u8], start: usize, end: usize) -> Option<Vec<(BoxId, usize, usize)>> {
    let mut out = Vec::new();
    let mut p = start;
    while p < end {
        let (sz, _) = mp4_box_size(buf, p, end)?;
        if sz == 0 {
            return None;
        }
        let e = (p as u64 + sz) as usize;
        if e > end {
            return None;
        }
        let mut t = [0u8; 4];
        t.copy_from_slice(&buf[p + 4..p + 8]);
        out.push((t, p, e));
        p = e;
    }
    Some(out)
}

fn mp4_box(typ: &BoxId, payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(payload.len() + 8);
    v.extend_from_slice(&((payload.len() + 8) as u32).to_be_bytes());
    v.extend_from_slice(typ);
    v.extend_from_slice(payload);
    v
}

/// data box:type 1=UTF-8 文本,0=二进制(如 trkn);locale 恒 0。
fn mp4_data(dtype: u32, payload: &[u8]) -> Vec<u8> {
    let mut p = Vec::with_capacity(payload.len() + 8);
    p.extend_from_slice(&dtype.to_be_bytes());
    p.extend_from_slice(&0u32.to_be_bytes());
    p.extend_from_slice(payload);
    mp4_box(b"data", &p)
}

fn mp4_text_item(typ: &BoxId, s: &str) -> Vec<u8> {
    mp4_box(typ, &mp4_data(1, s.as_bytes()))
}

fn mp4_trkn_item(track: u16, total: u16) -> Vec<u8> {
    let mut p = [0u8; 8];
    p[2..4].copy_from_slice(&track.to_be_bytes());
    p[4..6].copy_from_slice(&total.to_be_bytes());
    mp4_box(b"trkn", &mp4_data(0, &p))
}

/// meta 缺 hdlr 时的最小补位(handler 'mdir')。
fn mp4_mdir_hdlr() -> Vec<u8> {
    let mut p = vec![0u8; 8]; // version/flags + pre_defined
    p.extend_from_slice(b"mdir");
    p.extend_from_slice(&[0u8; 12]); // reserved
    p.push(0); // 空 handler 名 + null
    mp4_box(b"hdlr", &p)
}

const TAG_TITLE: BoxId = [0xA9, b'n', b'a', b'm']; // ©nam
const TAG_ALBUM: BoxId = [0xA9, b'a', b'l', b'b']; // ©alb
const TAG_ARTIST: BoxId = [0xA9, b'A', b'R', b'T']; // ©ART
const TAG_TRACK: BoxId = *b"trkn";

/// 生成新的 meta payload:保留旧 hdlr 与未知条目,替换我们管的 4 个键。
/// old 为旧 meta payload(含 version/flags),缺失传 None。
fn new_meta_payload(
    old: Option<&[u8]>,
    title: &str,
    album: &str,
    artist: &str,
    track: u16,
    total: u16,
) -> Result<Vec<u8>, String> {
    let mut hdlr: Option<Vec<u8>> = None;
    let mut meta_others: Vec<Vec<u8>> = Vec::new();
    let mut kept_items: Vec<Vec<u8>> = Vec::new();
    if let Some(p) = old {
        if p.len() < 4 {
            return Err("meta 过短".into());
        }
        let kids = mp4_children(p, 4, p.len()).ok_or("meta 解析失败")?;
        for (t, s, e) in &kids {
            if t == b"ilst" {
                let items = mp4_children(p, s + 8, *e).ok_or("ilst 解析失败")?;
                for (it, is, ie) in &items {
                    if *it == TAG_TITLE || *it == TAG_ALBUM || *it == TAG_ARTIST || *it == TAG_TRACK
                    {
                        continue; // 旧值丢弃,稍后写新值(重复打标签不叠加)
                    }
                    kept_items.push(p[*is..*ie].to_vec());
                }
            } else if t == b"hdlr" && hdlr.is_none() {
                hdlr = Some(p[*s..*e].to_vec());
            } else {
                meta_others.push(p[*s..*e].to_vec());
            }
        }
    }
    let mut ilst_p = Vec::new();
    for k in &kept_items {
        ilst_p.extend_from_slice(k);
    }
    ilst_p.extend_from_slice(&mp4_text_item(&TAG_TITLE, title));
    ilst_p.extend_from_slice(&mp4_text_item(&TAG_ALBUM, album));
    ilst_p.extend_from_slice(&mp4_text_item(&TAG_ARTIST, artist));
    ilst_p.extend_from_slice(&mp4_trkn_item(track, total));
    let mut out = Vec::new();
    if let Some(p) = old {
        out.extend_from_slice(&p[0..4]); // 沿用 version/flags
    } else {
        out.extend_from_slice(&[0u8; 4]);
    }
    out.extend_from_slice(&hdlr.unwrap_or_else(mp4_mdir_hdlr));
    for m in &meta_others {
        out.extend_from_slice(m);
    }
    out.extend_from_slice(&mp4_box(b"ilst", &ilst_p));
    Ok(out)
}

/// 重建 udta box:替换/追加 meta,其余子 box 原样保留。
fn rebuild_udta(
    buf: &[u8],
    s: usize,
    e: usize,
    title: &str,
    album: &str,
    artist: &str,
    track: u16,
    total: u16,
) -> Result<Vec<u8>, String> {
    let kids = mp4_children(buf, s + 8, e).ok_or("udta 解析失败")?;
    let mut payload = Vec::new();
    let mut done = false;
    for (t, cs, ce) in &kids {
        if t == b"meta" && !done {
            done = true;
            let mp = new_meta_payload(Some(&buf[cs + 8..*ce]), title, album, artist, track, total)?;
            payload.extend_from_slice(&mp4_box(b"meta", &mp));
        } else {
            payload.extend_from_slice(&buf[*cs..*ce]);
        }
    }
    if !done {
        let mp = new_meta_payload(None, title, album, artist, track, total)?;
        payload.extend_from_slice(&mp4_box(b"meta", &mp));
    }
    Ok(mp4_box(b"udta", &payload))
}

/// moov 内 stco/co64 chunk 偏移修正:旧 moov 终点之后的偏移整体平移 delta。
fn fix_chunk_offsets(
    buf: &mut [u8],
    moov_off: usize,
    moov_end_old: usize,
    delta: i64,
) -> Result<(), String> {
    let end = buf.len();
    let (mo, me) = mp4_find(buf, 0, end, b"moov").ok_or("moov 丢失")?;
    if mo != moov_off {
        return Err("moov 位置漂移".into());
    }
    let traks = mp4_children(buf, mo + 8, me).ok_or("moov 解析失败")?;
    for (t, ts, te) in &traks {
        if t != b"trak" {
            continue;
        }
        let mdia = mp4_find(buf, ts + 8, *te, b"mdia").ok_or("缺少 mdia")?;
        let minf = mp4_find(buf, mdia.0 + 8, mdia.1, b"minf").ok_or("缺少 minf")?;
        let stbl = mp4_find(buf, minf.0 + 8, minf.1, b"stbl").ok_or("缺少 stbl")?;
        let entries = mp4_children(buf, stbl.0 + 8, stbl.1).ok_or("stbl 解析失败")?;
        for (et, es, ee) in &entries {
            if et != b"stco" && et != b"co64" {
                continue;
            }
            let width = if et == b"stco" { 4 } else { 8 };
            if es + 16 > *ee {
                return Err("stco/co64 过短".into());
            }
            let n = u32::from_be_bytes(buf[es + 12..es + 16].try_into().map_err(|_| "stco 计数坏")?);
            for i in 0..n as usize {
                let p = es + 16 + i * width;
                if p + width > *ee {
                    return Err("chunk 偏移表越界".into());
                }
                if width == 4 {
                    let v = u32::from_be_bytes(buf[p..p + 4].try_into().unwrap());
                    if (v as usize) >= moov_end_old {
                        let nv = (v as i64 + delta) as u32;
                        buf[p..p + 4].copy_from_slice(&nv.to_be_bytes());
                    }
                } else {
                    let v = u64::from_be_bytes(buf[p..p + 8].try_into().unwrap());
                    if v >= moov_end_old as u64 {
                        let nv = (v as i64 + delta) as u64;
                        buf[p..p + 8].copy_from_slice(&nv.to_be_bytes());
                    }
                }
            }
        }
    }
    Ok(())
}

/// 给 m4a 写标签(title/album/artist/track):只改 box,不重编码。
/// moov 变长时同步修正 stco/co64,音频一字节不动。
fn tag_m4a(
    path: &Path,
    title: &str,
    album: &str,
    artist: &str,
    track: u16,
    total: u16,
) -> Result<(), String> {
    let buf = std::fs::read(path).map_err(|e| format!("读m4a失败: {}", e))?;
    let end = buf.len();
    let (moov_off, moov_end) = mp4_find(&buf, 0, end, b"moov").ok_or("找不到 moov box")?;
    let kids = mp4_children(&buf, moov_off + 8, moov_end).ok_or("moov 解析失败")?;
    let mut moov_p = Vec::with_capacity(moov_end - moov_off);
    let mut found_udta = false;
    for (t, s, e) in &kids {
        if t == b"udta" && !found_udta {
            found_udta = true;
            moov_p.extend_from_slice(&rebuild_udta(&buf, *s, *e, title, album, artist, track, total)?);
        } else {
            moov_p.extend_from_slice(&buf[*s..*e]);
        }
    }
    if !found_udta {
        let mp = new_meta_payload(None, title, album, artist, track, total)?;
        let meta = mp4_box(b"meta", &mp);
        moov_p.extend_from_slice(&mp4_box(b"udta", &meta));
    }
    if moov_p.len() as u64 + 8 > u32::MAX as u64 {
        return Err("moov 过大".into());
    }
    let new_moov = mp4_box(b"moov", &moov_p);
    let delta = new_moov.len() as i64 - (moov_end - moov_off) as i64;
    let mut out = Vec::with_capacity(end + delta.max(0) as usize);
    out.extend_from_slice(&buf[..moov_off]);
    out.extend_from_slice(&new_moov);
    out.extend_from_slice(&buf[moov_end..]);
    if delta != 0 {
        fix_chunk_offsets(&mut out, moov_off, moov_end, delta)?;
    }
    std::fs::write(path, &out).map_err(|e| format!("写m4a失败: {}", e))?;
    Ok(())
}

#[tauri::command]
fn cancel_export() -> String {
    cancelled_flag().store(true, Ordering::SeqCst);
    cancel_notify().notify_waiters();
    "已取消".to_string()
}

#[tauri::command]
fn cpu_cores() -> usize {
    std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4)
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .invoke_handler(tauri::generate_handler![
            open_epub,
            get_segment,
            list_voices,
            preview_speech,
            start_export,
            cancel_export,
            cpu_cores
        ])
        .run(tauri::generate_context!())
        .expect("启动失败");
}

fn main() {
    run();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_real_epub() {
        let path = "/Users/lin/Documents/Code/llm-classical-player/人类简史三部曲尤瓦尔·赫拉利作品套装共3册（烧脑神书，颠覆你的世界观！错过这套书的人将错过未来）_尤瓦尔·赫拉利_z_library.epub";
        let book = open_epub_impl(path).expect("parse");
        assert!(!book.toc.is_empty(), "toc empty");
        assert!(!book.chapters.is_empty());
        println!("chapters={} toc={} title={}", book.chapters.len(), book.toc.len(), book.title);
        // NCX 标题应被采用(而非 text0000x 文件名回退)
        assert!(book.toc.iter().any(|e| e.title.contains("科技颠覆")), "ncx titles missing");
        // 每段都应切出非空正文(空章节/乱序不得撑出巨型区间)
        let mut biggest = 0usize;
        for ti in 0..book.toc.len() {
            let (_, texts) = segment_range(&book, ti);
            let chunks = chunk_paragraphs(&texts, 450);
            biggest = biggest.max(chunks.len());
            assert!(!chunks.is_empty(), "seg{} empty", ti);
        }
        println!("biggest segment chunks={}", biggest);
        assert!(biggest < 1000, "degenerate range");
        // 文件名规范:4位前导0-标题.m4a
        let f = format!("{:04}-{}.m4a", 1, sanitize_title(&book.toc[0].title));
        assert!(f.starts_with("0001-") && f.ends_with(".m4a"), "{}", f);
    }

    #[test]
    fn tag_m4a_roundtrip() {
        // 1秒 440Hz 正弦 wav(24k/mono/16bit) → afconvert → 打标签 → 结构校验
        let dir = std::env::temp_dir().join(format!("epub_tagtest_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let wav = dir.join("t.wav");
        let m4a = dir.join("t.m4a");
        let sr = 24000u32;
        let n = sr as usize;
        let mut pcm = Vec::with_capacity(n * 2);
        for i in 0..n {
            let v = ((i as f32 * 440.0 * std::f32::consts::TAU / sr as f32).sin() * 20000.0) as i16;
            pcm.extend_from_slice(&v.to_le_bytes());
        }
        super::write_wav(&wav, &pcm, sr).unwrap();
        let conv = std::process::Command::new("afconvert")
            .arg("-f").arg("m4af").arg("-d").arg("aac")
            .arg("-b").arg("64000").arg(&wav).arg(&m4a)
            .output();
        let Ok(conv) = conv else {
            println!("no afconvert, skip");
            return;
        };
        assert!(conv.status.success(), "afconvert failed");
        let before = std::fs::read(&m4a).unwrap();
        let (moov_off, moov_end) = super::mp4_find(&before, 0, before.len(), b"moov").unwrap();
        // 旧 stco 快照
        let old_entries = collect_stco(&before);
        assert!(!old_entries.is_empty());
        let title = "第1章 测试标题·дин";
        super::tag_m4a(&m4a, title, "测试书", "epub", 7, 34).unwrap();
        // 再打一次:键不得重复
        super::tag_m4a(&m4a, title, "测试书", "epub", 7, 34).unwrap();
        let after = std::fs::read(&m4a).unwrap();
        let delta = after.len() as i64 - before.len() as i64;
        println!("tag delta={} bytes", delta);
        // ilst 校验
        let (mo2, me2) = super::mp4_find(&after, 0, after.len(), b"moov").unwrap();
        assert_eq!(mo2, moov_off);
        let kids = super::mp4_children(&after, mo2 + 8, me2).unwrap();
        let mut ilst_raw = None;
        for (t, s, e) in &kids {
            if t == b"udta" {
                let uk = super::mp4_children(&after, s + 8, *e).unwrap();
                for (mt, ms, me) in &uk {
                    if mt == b"meta" {
                        let mk = super::mp4_children(&after, ms + 8 + 4, *me).unwrap();
                        for (it2, is, ie) in &mk {
                            if it2 == b"ilst" {
                                ilst_raw = Some(after[*is..*ie].to_vec());
                            }
                        }
                    }
                }
            }
        }
        let ilst = ilst_raw.expect("ilst missing");
        let items = super::mp4_children(&ilst, 8, ilst.len()).unwrap();
        let mut names = 0;
        let mut trkn_ok = false;
        for (t, s, e) in &items {
            if t == b"\xA9nam" {
                names += 1;
                assert!(ilst[*s..*e].windows(title.as_bytes().len()).any(|w| w == title.as_bytes()));
            }
            if t == b"trkn" {
                // item头8B + data头8B + type4B + locale4B 之后才是 8B 负载
                let payload = &ilst[s + 24..*e];
                assert_eq!(&payload[2..4], &7u16.to_be_bytes());
                assert_eq!(&payload[4..6], &34u16.to_be_bytes());
                trkn_ok = true;
            }
        }
        assert_eq!(names, 1, "title must appear exactly once");
        assert!(trkn_ok, "trkn missing");
        // stco:旧终点之后的条目整体平移 delta,其余不动
        let new_entries = collect_stco(&after);
        assert_eq!(old_entries.len(), new_entries.len());
        for (o, a) in old_entries.iter().zip(new_entries.iter()) {
            let expect = if (*o as usize) >= moov_end { (*o as i64 + delta) as u64 } else { *o };
            assert_eq!(*a, expect, "stco shift wrong");
        }
        // 音频数据本身逐字节保留(mdat 区域在新文件中原样出现)
        let (_, mdat_end_old) = super::mp4_find(&before, 0, before.len(), b"mdat").unwrap();
        let mdat_old = &before[moov_end..mdat_end_old.min(before.len())];
        assert!(after.windows(mdat_old.len().min(4096)).any(|w| w == &mdat_old[..mdat_old.len().min(4096)]));
        std::fs::remove_dir_all(&dir).ok();
    }

    fn collect_stco(buf: &[u8]) -> Vec<u64> {
        let mut out = Vec::new();
        let (mo, me) = super::mp4_find(buf, 0, buf.len(), b"moov").unwrap();
        let traks = super::mp4_children(buf, mo + 8, me).unwrap();
        for (t, ts, te) in &traks {
            if t != b"trak" {
                continue;
            }
            let mdia = super::mp4_find(buf, ts + 8, *te, b"mdia").unwrap();
            let minf = super::mp4_find(buf, mdia.0 + 8, mdia.1, b"minf").unwrap();
            let stbl = super::mp4_find(buf, minf.0 + 8, minf.1, b"stbl").unwrap();
            let kids = super::mp4_children(buf, stbl.0 + 8, stbl.1).unwrap();
            for (et, es, ee) in &kids {
                if et != b"stco" {
                    continue;
                }
                let n = u32::from_be_bytes(buf[es + 12..es + 16].try_into().unwrap());
                for i in 0..n as usize {
                    out.push(u32::from_be_bytes(buf[es + 16 + i * 4..es + 20 + i * 4].try_into().unwrap()) as u64);
                }
                let _ = ee;
            }
        }
        out
    }

    #[test]
    fn parse_oxford_100() {
        let path = "/Users/lin/Documents/Code/llm-classical-player/牛津通识读本百本纪念套装（共100册）（全球精英学者撰写的通识文库，被誉为真正的“大家小书”，带你读遍全球名校的专业入门课！）.epub";
        let t0 = std::time::Instant::now();
        let book = open_epub_impl(path).expect("parse");
        println!("oxford parse {:?} chapters={} toc={}", t0.elapsed(), book.chapters.len(), book.toc.len());
        assert!(!book.toc.is_empty());
        let mut lv = [0usize; 7];
        for e in &book.toc {
            lv[e.level.min(6)] += 1;
        }
        println!("oxford levels={:?}", lv);
        // 默认展开可见数(沿用前端 computeDepth 口径:展开 level<=d)
        fn visible(toc: &[super::TocEntry], d: usize) -> usize {
            let mut stack: Vec<(usize, usize)> = Vec::new();
            let mut n = 0;
            for (i, e) in toc.iter().enumerate() {
                while stack.last().map(|x: &(usize, usize)| x.1).unwrap_or(0) >= e.level {
                    stack.pop();
                }
                if stack.iter().all(|(idx, _)| toc[*idx].level <= d) {
                    n += 1;
                }
                stack.push((i, e.level));
            }
            n
        }
        let mut depth = 6;
        while depth > 0 && visible(&book.toc, depth) > 200 {
            depth -= 1;
        }
        println!("oxford expandDepth={} visible={}", depth, visible(&book.toc, depth));
        assert!(visible(&book.toc, depth) <= 200);
        // 随机抽 3 段切分冒烟
        for ti in [0, book.toc.len() / 2, book.toc.len() - 1] {
            let (t, texts) = super::segment_range(&book, ti);
            let chunks = super::chunk_paragraphs(&texts, 450);
            println!("oxford seg{} {:?} texts={} chunks={}", ti,
                t.chars().take(20).collect::<String>(), texts.len(), chunks.len());
        }
    }
}
