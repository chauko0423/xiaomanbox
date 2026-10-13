# Task contracts

Exact English prompts used by the model are retained under source/operative_inputs/tasks. Chinese translations are for readers only.

## F10-E03 (express_3)

Editable: lib/response.js

Concise: Repair response content negotiation when only the default handler is supplied. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

Explicit: Repair response format negotiation. A format object containing only a default handler must invoke that handler instead of selecting an unrelated representation. Preserve supported named representations, normal Accept-header negotiation, Vary behavior and existing not-acceptable handling. Preserve the exported API. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

## F10-E05 (express_5)

Editable: lib/utils.js

Concise: Repair recognition of Windows absolute paths with forward slashes. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

Explicit: Repair the absolute-path predicate so a Windows drive-qualified path with a colon and forward slash is recognized as absolute, consistently with the existing drive-qualified backslash form. Preserve existing Unix absolute paths, UNC paths and rejection of relative paths. Preserve the exported API. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

## F10-X05 (hexo_5)

Editable: lib/plugins/helper/is.js

Concise: Repair the current-page helper for root pages and optional leading slashes. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

Explicit: Repair current-page matching. Paths with and without a leading slash must refer to the same page. An omitted path targets the home page rather than every page; in non-strict matching, root matches the home index and must not mark unrelated pages current. Strict matching must preserve index.html expansion for trailing slashes and exact page matching. Preserve other helpers and valid nested-page matching. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

## F10-X07 (hexo_7)

Editable: lib/plugins/helper/toc.js

Concise: Repair table-of-contents labels for headings containing HTML markup. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

Explicit: Repair table-of-contents heading labels. Labels must use readable heading text, omitting embedded markup elements, with text escaped so special characters remain safe literal HTML text in the resulting links. Preserve heading IDs and link targets, numbering options, nesting and other valid table-of-contents behavior. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

## F10-H07 (hessian.js_7)

Editable: lib/object.js, lib/v1/encoder.js

Concise: Repair encoding of typed java.util.Map values. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

Explicit: Repair map serialization so typed java.util.Map values use the same untyped-map representation as java.util.HashMap values. Preserve key/value payloads, ordinary maps, custom typed maps and other Hessian encoding behavior. Retain the public default-class mapping and encoder APIs. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

## F10-H08 (hessian.js_8)

Editable: lib/utils.js

Concise: Repair conversion of decoded long integers at the JavaScript safe-integer boundary. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.

Explicit: Repair decoded-long conversion. Values within the inclusive signed JavaScript safe-integer interval must remain numeric; values outside that interval must become exact decimal strings. Handle positive and negative boundaries without rounding large integers. Preserve other serialization and decoding behavior. Only edit the declared localized source files. No reference code or test source is supplied. Apply actual edits and explicitly finish. Original pending cases remain pending. Full original developer tests run once after interaction irreversibly closes and never become repair feedback. All declared resource and tool caps apply.
