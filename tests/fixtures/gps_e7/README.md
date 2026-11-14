# aasdk's location schema

`LocationData.proto` is aasdk's file, copied without changes from opencardev/aasdk at commit
`f9cf21e2fd3bfb8a43e61601c84d9b1b96389ff0`. `sources.json` records its URL and SHA-256. aasdk is under the GNU General
Public License v3.0, and so is this file. The licence of this repository doesn't cover it.

Only a PC test reads it. Nothing on the head unit is built from it, and no release contains it.

`tests/test_gps_wire_oracle.py` compiles the schema with Google's `protoc`, writes the expected messages for a list of
coordinates given as literal E7 values, and checks that the project's C++ encoder sends the same bytes, on the PC and,
with `--arm`, under QEMU. The car's GPS reader keeps coordinates in E6 internally and converts them to E7 when it
builds the Android Auto message, so an encoder that forgot the conversion would be off by a factor of 10 and fail.
