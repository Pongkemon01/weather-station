Robin Weather Station Configurator
==================================

Version:      1.0.0
Manufacturer: RobinLab-KU


About
-----

This application is the configuration and monitoring utility for the
Robin Weather Station. It communicates with the device over USB.


Installation
------------

No installation is required.

   1. Extract this folder to any location you like (your Desktop, a USB
      stick, or a network share all work).
   2. Connect the Robin Weather Station to your computer via USB.
   3. Double-click "robin_wsc.exe".

To remove the application, simply delete this folder. No registry entries
or system files are created.


Supported operating systems
---------------------------

   * Windows 10 (build 1809 or later)
   * Windows 11


USB device identification
-------------------------

The Robin Weather Station identifies itself with the following USB IDs:

   Vendor ID:  0x1209  (pid.codes open hardware allocation)
   Product ID: 0xDCB1

The application searches for any connected device with this VID/PID pair.


Open source acknowledgements
----------------------------

This application is built with the Qt framework (https://www.qt.io/),
used under the terms of the GNU Lesser General Public License v3 (LGPLv3).
See COPYING.LGPLv3.txt in this folder for the full Qt license text.

Qt source code is available from https://www.qt.io/download-open-source

The Qt libraries distributed with this application (Qt6Core.dll,
Qt6Gui.dll, Qt6Widgets.dll, Qt6SerialPort.dll, and the qwindows.dll
platform plugin) are unmodified copies of the official Qt 6 release.


Support
-------

For support, please contact RobinLab-KU.
