# Tibia FPS Unlimiter

Even with unlimited FPS in the options, the Tibia client stops at around 200 fps. Qt is the cause:
before each frame it waits on a 2 to 5 ms timer. This program sets that wait to zero.

Works with clients built on Qt 6 (64-bit) and Qt 5 (32-bit), official or OT. Windows only.

## Usage

Run `TibiaFpsUnlimiter.exe`.

- Client already running: select it in the list and click "Remove limit". The "Qt limit" column
  shows whether the limit is already gone.
- New client: choose the executable and click "Launch unlimited".

With a path as argument, the client starts directly, without the window:

```
TibiaFpsUnlimiter.exe "C:\path\to\client.exe"
```

A client started by the server's launcher comes with the limit. Use "Remove limit" once it is open.

## How it works

The client asks for each frame with `QWindow::requestUpdate()`, and Qt delays the request with a
timer: 5 ms on Qt 5, and on Qt 6.9, 5 ms divided by (monitor refresh rate / 60). The environment
variable `QT_QPA_UPDATE_IDLE_TIME` overrides that time.

- New client: the program starts it with `QT_QPA_UPDATE_IDLE_TIME=0`.
- Running client: Qt has already read the variable and stored the result inside `Qt5Gui.dll` or
  `Qt6Gui.dll`. The program finds that address from the code that reads the variable and writes 0
  into the client's memory. If the code does not match a known shape, nothing is written.

No client file is modified.

Tested on clients with Qt 5.15 (32-bit), 6.8 and 6.9. The address search also recognizes Qt 6.5,
6.10 and 6.11, but was not tested on a running client.

Writing to the client's memory may be flagged by a server's anticheat. "Launch unlimited" does not
touch memory.

## Building

Visual Studio with C++ (C++20). `.\build.ps1` produces `bin\TibiaFpsUnlimiter.exe`.

## License

MIT. See `LICENSE`.
