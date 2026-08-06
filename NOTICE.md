# Third-party components and lineage

This project is licensed **GPL-3.0** as a whole because it vendors the UxPlay
core. Individual components:

| Component | Path | License | Origin |
|---|---|---|---|
| UxPlay RAOP/mirroring core | `third_party/uxplay/lib` | GPL-3.0 | [FDH2/UxPlay](https://github.com/FDH2/UxPlay), itself derived from RPiPlay → AirplayServer → shairplay lineage |
| libplist | `third_party/libplist` | LGPL-2.1 | [libimobiledevice/libplist](https://github.com/libimobiledevice/libplist) 2.6.0 |
| llhttp, playfair | inside `third_party/uxplay/lib` | MIT / as upstream | bundled by UxPlay |
| Win32 pthread/unistd shim | `third_party/win_compat` | GPL-3.0 (this project) | written for the vkrender MSVC port |
| Receiver (`src/`) | `src/` | GPL-3.0 (this project) | vkrender project |
| vkrender producer SDK | `sdk/` | vkrender project terms (permissive); **not** part of the GPL work | [jsuppe/vkrender](https://github.com/jsuppe/vkrender) |

Windows-specific patches carried in the vendored trees (all guarded with
`_MSC_VER`/`WIN32`): MSVC-safe replacements for GNU statement-expression
macros in libplist, `snprintf`/`SOL_TCP` compatibility, compound-literal
fixes in `crypto.c`, NULL-guards for optional callbacks in
`raop_rtp_mirror.c`, and lazy-init pthread mutex/cond semantics in the shim
(zero-initialized statics must be lockable, as with
`PTHREAD_MUTEX_INITIALIZER`).

GStreamer and OpenSSL are external runtime dependencies (LGPL / Apache-2.0)
installed via the GStreamer MSVC distribution; they are not redistributed
here.
