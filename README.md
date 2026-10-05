# Nada

**A lightweight Windows utility for finding and sweeping away empty folders.**

Nada scans a directory, identifies folders that are genuinely empty — including folders containing only other empty folders — and lets you review and safely remove them.

Built as a small, native **Win32 C++ application**, Nada is designed to be fast, responsive, and simple to use.

## Features

- 🔎 **Fast directory scanning** — recursively scans a selected folder for empty directories.
- 🧹 **Nested empty-folder detection** — detects folders that contain only other empty folders.
- ⚡ **Background processing** — scanning and deletion run on a worker thread so the UI remains responsive.
- 📊 **Live progress** — displays scanning and deletion progress while operations are running.
- 🛑 **Cancelable operations** — scanning and deletion can be stopped while in progress.
- 🛡️ **Protect folders** — mark individual results as protected so they will not be deleted.
- 🌳 **Automatic parent protection** — a parent folder containing a protected folder is automatically kept.
- 📋 **Review before deletion** — results are displayed in a list before anything is removed.
- 🖱️ **Multiple ways to protect folders** — use the button, context menu, double-click, or `Space`.
- 🚫 **Configurable skip rules** — exclude Windows and program folders from scans.
- 📁 **Custom skip list** — add your own directories that should never be scanned.
- 💾 **Persistent settings** — preferences are stored in `%APPDATA%\Nada\settings.ini`.
- 🖱️ **Drag and drop** — drop a folder onto the application to scan it.
- 🖥️ **DPI-aware interface** — designed to scale correctly on modern Windows displays.
- 📦 **Windows installer** — includes an Inno Setup installer configuration.

## Safety

Nada is deliberately conservative when deciding whether a folder is empty.

Folders are only considered removable when they contain no files or other meaningful content. Nested empty directories are handled from the deepest level upward, allowing them to be removed safely.

**Symbolic links and junctions are not followed** and are treated as content rather than empty directories.

Nada also provides built-in protection for commonly sensitive Windows locations, including:

- Windows system directories
- `Program Files`
- `Program Files (x86)`
- `ProgramData`
- `AppData`
- Other user-defined locations

Before deletion, the application shows the number of folders that will be removed and asks for confirmation.

> **Always review the results before confirming deletion, especially when scanning an entire drive.**

## How It Works

1. Choose a directory to scan.
2. Nada recursively examines its subdirectories.
3. Empty folders are collected in deepest-first order.
4. Results are displayed for review.
5. Protect any folders you want to keep.
6. Click **Delete**.
7. Nada removes the remaining empty directories.

The deepest-first approach is important for nested empty directories: child folders are removed before their parents.

## Building

Nada is a native C++17 Windows application and can be built with either **Visual Studio/MSVC** or **MSYS2/MinGW-w64**.

### MSVC

Run from a **Visual Studio Developer Command Prompt**:

```bat
build_msvc.bat
```

This builds `Nada.exe` using:

- C++17
- MSVC
- Static runtime (`/MT`)
- Unicode
- Native Windows subsystem

### MinGW-w64

Run from an **MSYS2 UCRT64 terminal**:

```sh
./build_mingw.sh
```

The MinGW build uses:

- C++17
- MinGW-w64
- Windows GUI subsystem
- Static linking
- Native Win32 libraries

## Requirements

- Windows 10 or later
- C++17-compatible Windows compiler for building
- Visual Studio or MSYS2/MinGW-w64

No external runtime or third-party framework is required by the application.

## Project Structure

```text
.
├── Nada.cpp          # Main application source
├── Nada.rc           # Windows resources
├── Nada.manifest     # Application manifest
├── nada.ico          # Application icon
├── Nada.iss          # Inno Setup installer configuration
├── build_msvc.bat    # MSVC build script
└── build_mingw.sh    # MinGW/MSYS2 build script
```

## Technology

Nada is intentionally built using the Windows platform APIs rather than a large GUI framework.

- **C++17**
- **Win32 API**
- `std::filesystem`
- Windows Common Controls
- Windows Shell APIs
- Native worker threads
- Inno Setup

This keeps the application small, portable, and lightweight.

## Configuration

Nada stores its settings in:

```text
%APPDATA%\Nada\settings.ini
```

The configuration includes:

- Windows folder protection
- Program folder protection
- Custom folders to skip

Changes to the skip configuration are applied to the next scan.
