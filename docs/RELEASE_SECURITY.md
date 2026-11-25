# Release security checks

Run these on the final artifacts, after the last package run. Repackaging changes the ZIP and AppImage hashes, so a
scan only counts for the exact file that gets published.

A clean ZIP scan does not cover the programs inside it: an engine can flag an executable in a ZIP that scanned clean.
Scan each file below on its own. Uploads stay manual: no script uploads, publishes or tags anything.

| File | Why |
| --- | --- |
| `dist\SonsOfSpartaPC-v<version>-windows-x64.zip` | What Windows players download |
| `dist\SonsOfSpartaPC-v<version>-x86_64.AppImage` | What Linux players download |
| `dist\SonsOfSpartaPC-v<version>-windows-x64\sons_of_sparta.exe` | The one program players open: launcher and game |
| `dist\SonsOfSpartaPC-v<version>-windows-x64\trg-updater.exe` | The update helper that installs a newer release |

The two executables come from the staged folder `dist\SonsOfSpartaPC-v<version>-windows-x64\`, which `tools\package.ps1`
keeps after packaging: the ZIP is made from exactly that folder, and `tools\package-release.ps1` checks both again with
`tools\package.ps1 -VerifyOnly`. Nothing has to be extracted by hand and no second copy exists. The AppImage is one file:
scan it as published. The debug symbols are in `dist\SonsOfSpartaPC-v<version>-windows-x64-symbols\`, outside the folder
and the ZIP, and are not part of the scan.

`tools\package-release.ps1` writes the SHA-256 of the four files into `dist\release-notes-v<version>.md`,
`dist\virustotal-v<version>.json` and `dist\virustotal-v<version>.txt`, and a full run writes every VirusTotal entry as
PENDING: the notes then say so and must not be published. After the scans:

1. In `dist\virustotal-v<version>.json`, replace each PENDING with the URL of that file's scan,
   `https://www.virustotal.com/gui/file/<sha256>` (optionally ending in `?nocache=1`).
2. Run `tools\package-release.ps1 -NotesOnly`. It builds, tests and packages nothing and never moves or deletes an
   artifact. It checks the package checksums and that the staged folder still matches the ZIP, keeps only URLs that name
   the exact SHA-256 of their file, and rewrites the notes and both manifests. Only when all four URLs are valid do the
   notes say the exact final files were checked with VirusTotal, and link each scan.

The DLLs in the ZIP are third-party files copied unchanged; scan them separately only if an engine names one.

## Release report

Record all four values before publishing, and compare each VirusTotal hash with the SHA-256 in the release notes.

| File | SHA-256 | VirusTotal URL |
| --- | --- | --- |
| Windows ZIP | | |
| Linux AppImage | | |
| `sons_of_sparta.exe` | | |
| `trg-updater.exe` | | |

## Before publishing

Do not publish while an engine flags any of these files, unless the detection has been investigated to a conclusion
and that conclusion is written down.

Never work around a detection by packing, obfuscating or encrypting the program, by disabling a security product or
by asking players to add an exclusion.

Write no more into the release notes than the scans show: record each scan's URL, and give a detection count only when it
was read from that scan. A scan is an additional check, not a guarantee of safety.
