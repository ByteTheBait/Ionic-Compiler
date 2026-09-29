//! Text-level relative-import preprocessor.
//!
//! The self-hosted compiler never feeds the parser a single file: it first
//! splices the source tree together by expanding `import "rel/path.ionic";`
//! statements (resolved relative to the *importing* file) into their file
//! contents, depth-first, before lexing the combined source. This mirrors the
//! `imp_expand` / `imp_resolve_rel` logic in `src/imports.ionic` so the Rust
//! bootstrap can assemble `src/main.ionic` exactly the way the self-hosted
//! compiler does.
//!
//! Dotted imports (`import std.math.*;`) are intentionally *left in place*:
//! they are handled later by the AST-level selective-compilation pass in
//! `imports.rs`, which reads them off the parsed `Program.imports`.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

/// State for one expansion run.
struct Resolver {
    parts: Vec<String>,           // accumulated fragments, dependency order
    loaded: HashSet<String>,      // canonical module paths already spliced
}

/// Collapse `.` and `..` segments so a diamond import loads a file once.
fn norm(path: &str) -> String {
    let abs = path.starts_with('/');
    let mut out: Vec<&str> = Vec::new();
    for seg in path.split('/') {
        match seg {
            "" | "." => {}
            ".." => { out.pop(); }
            s => out.push(s),
        }
    }
    let joined = out.join("/");
    if abs {
        format!("/{}", joined)
    } else if joined.is_empty() {
        ".".to_string()
    } else {
        joined
    }
}

/// Directory portion of a path ("a/b/c.ionic" -> "a/b"; "c.ionic" -> ".").
fn dir_of(path: &str) -> String {
    match path.rfind('/') {
        None => ".".to_string(),
        Some(0) => "/".to_string(),
        Some(i) => path[..i].to_string(),
    }
}

/// Join a directory with a relative path; an absolute `rel` passes through.
fn join(dir: &str, rel: &str) -> String {
    if rel.starts_with('/') {
        return rel.to_string();
    }
    if dir.is_empty() || dir == "." {
        return rel.to_string();
    }
    format!("{}/{}", dir, rel)
}

impl Resolver {
    fn new() -> Self {
        Resolver { parts: Vec::new(), loaded: HashSet::new() }
    }

    /// Resolve + expand a path-style (string literal) import, relative to `cur_dir`.
    fn resolve_rel(&mut self, rel: &str, cur_dir: &str) -> Result<(), String> {
        let full = norm(&join(cur_dir, rel));
        if self.loaded.contains(&full) {
            return Ok(());
        }
        self.loaded.insert(full.clone());

        let content = std::fs::read_to_string(&full).map_err(|_| {
            format!("ionic: cannot resolve import '{}'\n  tried: {}", rel, full)
        })?;

        // Nested imports resolve relative to the imported file's own directory.
        let next_dir = dir_of(&full);
        self.expand(&content, &next_dir)?;
        Ok(())
    }

    /// Splice a source text: recursively inline relative imports, keep dotted
    /// imports verbatim, and accumulate everything else as a fragment.
    fn expand(&mut self, src: &str, cur_dir: &str) -> Result<(), String> {
        let bytes = src.as_bytes();
        let n = bytes.len();
        let mut acc = String::new();
        let mut pos = 0;

        while pos < n {
            // Byte offset of the next newline (or EOF).
            let nl = match src[pos..].find('\n') {
                Some(off) => pos + off,
                None => n,
            };
            let s0 = skip_space(bytes, pos, nl);

            let mut is_imp = false;
            if s0 < nl && starts_at(bytes, "import", s0) {
                let after = s0 + 6;
                if after < n {
                    let c = bytes[after];
                    if c == b' ' || c == b'\t' {
                        is_imp = true;
                    }
                }
            }

            if is_imp {
                let p = skip_space(bytes, s0 + 6, n);
                if p < n && bytes[p] == b'"' {
                    // Path-style: `import "codegen/native.ionic";`
                    let qstart = p + 1;
                    let mut q = qstart;
                    while q < n && bytes[q] != b'"' {
                        q += 1;
                    }
                    let rel = &src[qstart..q];
                    self.resolve_rel(rel, cur_dir)?;
                } else {
                    // Dotted module path — leave for the AST-level pass.
                    acc.push_str(&src[pos..nl]);
                    acc.push('\n');
                }
            } else {
                acc.push_str(&src[pos..nl]);
                acc.push('\n');
            }
            pos = nl + 1;
        }

        if !acc.is_empty() {
            self.parts.push(acc);
        }
        Ok(())
    }
}

fn skip_space(bytes: &[u8], from: usize, limit: usize) -> usize {
    let mut i = from;
    let end = limit.min(bytes.len());
    while i < end && (bytes[i] == b' ' || bytes[i] == b'\t') {
        i += 1;
    }
    i
}

fn starts_at(bytes: &[u8], prefix: &str, at: usize) -> bool {
    let p = prefix.as_bytes();
    if bytes.len() < at + p.len() {
        return false;
    }
    &bytes[at..at + p.len()] == p
}

/// True if `src` contains a relative (string-literal) import, i.e. the source
/// needs splicing. Single-file programs without such imports are left exactly
/// as written so their line numbers survive into diagnostics.
pub fn has_relative_import(src: &str) -> bool {
    let bytes = src.as_bytes();
    let n = bytes.len();
    let mut pos = 0;
    while pos < n {
        let nl = match src[pos..].find('\n') {
            Some(off) => pos + off,
            None => n,
        };
        let s0 = skip_space(bytes, pos, nl);
        if s0 < nl && starts_at(bytes, "import", s0) {
            let after = s0 + 6;
            if after < n && (bytes[after] == b' ' || bytes[after] == b'\t') {
                let p = skip_space(bytes, after, n);
                if p < n && bytes[p] == b'"' {
                    return true;
                }
            }
        }
        pos = nl + 1;
    }
    false
}

/// Expand all relative imports reachable from `entry`, returning the combined
/// source (fragments in depth-first, dependency-first order). Dotted imports
/// are preserved so the AST-level resolver still sees them.
pub fn expand_entry(entry: &str) -> Result<String, String> {
    let mut r = Resolver::new();
    let entry_path = Path::new(entry);
    let entry_dir = entry_path
        .parent()
        .map(|p| p.to_string_lossy().into_owned())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| ".".to_string());

    let content = std::fs::read_to_string(entry)
        .map_err(|e| format!("Cannot read '{}': {}", entry, e))?;

    // Mark the entry so a module that imports it back doesn't loop.
    let full = norm(&PathBuf::from(entry).to_string_lossy());
    r.loaded.insert(full);

    r.expand(&content, &entry_dir)?;
    Ok(r.parts.join("\n"))
}
