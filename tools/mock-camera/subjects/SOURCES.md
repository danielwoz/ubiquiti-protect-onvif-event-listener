# Subject images

The scenario API shows one of these in the video and snapshot, so NanoDet-M has a real
subject to classify. `make-subjects.sh` turns each into a 1280x720 RGBA overlay when the
image is built.

| Overlay | Source | Licence |
|---|---|---|
| person | `test/testdata/security_cam_person.jpg` (copied in by `build.sh`) | Public domain |
| vehicle | `test/testdata/security_cam_vehicle.jpg` (copied in by `build.sh`) | Public domain |
| animal | `animal.jpg`: "Male German Shepherd dog" by Hans Kemperman, via Pixabay, from [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:German-shepherd-4055031920.jpg); the 1280 px Commons thumbnail scaled to 1080 px | CC0 1.0 (public domain dedication) |
| package | drawn by `make-subjects.sh` (a cardboard box on a doorstep) | not applicable |
