# Native guest locale preference

The runtime has an explicit formatting-locale profile. Its default is `en-US`;
run modes can select a complete BCP47 tag with `--locale-name TAG` before guest
startup. This is a guest configuration choice. Host locale, geography, accounts
and UI settings are not queried. The CRT still starts in its independent C locale.

The installed ICU dependency validates the whole supplied tag and canonicalizes
language, script and region spelling. Invalid suffixes are rejected rather than
accepting a parsed prefix. Input and canonical output fit the Windows locale-name
capacity, including NUL. Selection is permitted once before its first read and
is protected by a mutex; each runtime owns its own profile. Canonicalization
checks name syntax and does not promise all Windows NLS data for custom locales.

`GetUserDefaultLocaleName` resolves through kernel32, kernelbase and the existing
localization contract. It copies the selected name as UTF-16 and returns the
count including NUL. Packed destinations are supported. Null output or capacity
below the required count returns zero with `ERROR_INSUFFICIENT_BUFFER`, without
partial writes or a zero-capacity sizing result. Successful calls preserve host
errno, guest errno and LastError. Allocation failure returns an explicit error.

Original fixtures cover default/configured/invalid/frozen profiles, separate
runtime preferences, exact/short/null/packed outputs and the real CLI-to-PE call.
Game startup is checked separately.

`LocaleNameToLCID` maps known specific names, the configured user default (null
name), the invariant empty name and known neutral names with the neutral flag.
ICU supplies Windows identifiers; an exact reverse-name check rejects its broader
language fallback for unsupported regions. The adapter preserves native and
guest error state on success. Malformed names, invalid flags and unterminated
bounded input return parameter errors. Unknown/custom mappings, system-default
selection, legacy Windows sort-name syntax and neutral-to-specific inference
stop explicitly. Other locale data/UI-language APIs remain separate work.
