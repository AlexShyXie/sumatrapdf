## First run: security warning

This build is not code-signed, so the first time you run a downloaded copy Windows shows a
one-time confirmation ("Unknown publisher" / "Windows protected your PC"). Unblock the file
once and it starts normally from then on. Any of these works:

1. Unblock the file: right-click `SumatraPDF-$VERSION.exe` → **Properties** → check
   **Unblock** → OK. Or run:
   `powershell -c "Unblock-File .\SumatraPDF-$VERSION.exe"`
2. Download with a command-line tool instead of the browser; such files carry no
   "from the internet" mark and never trigger the prompt:
   `curl.exe -LO https://github.com/$REPO_SLUG/releases/download/v$VERSION/SumatraPDF-$VERSION.exe`
3. Click through the dialog: **More info** → **Run anyway** (SmartScreen), or **Run**
   ("Open File - Security Warning").

SHA256 checksum of the file attached to this release, in case you want to verify the download:

```
$SHA256
```

Check locally with: `certutil -hashfile SumatraPDF-$VERSION.exe SHA256`
