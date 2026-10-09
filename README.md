# Qt® Multimedia

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/sparky-stereo-os/sparkyos-swirl-light-128.png">
  <img src=".github/sparky-stereo-os/sparkyos-swirl-dark-128.png" alt="SparkyOS" width="128" height="128">
</picture>

## Sparky Stereo OS

This is Sparky Stereo OS's stereo work on Qt® Multimedia, forked from [qt/qtmultimedia](https://github.com/qt/qtmultimedia).
It adds `QVideoFrameFormat::stereoMode` for every stereoscopic 3D packing that the H.264 and H.265 frame packing SEI and Matroska StereoMode define, and a VideoOutput that takes the views apart.
High bit depth video and audio keep their depth.

Where it comes from:

- [Qt](https://www.qt.io/) is made by The Qt Company and the contributors of the Qt Project.
- [Debian](https://www.debian.org/) is the base of the system.
- [SparkyLinux](https://sparkylinux.org/), by Paweł "pavroo" Pijanowski, builds on Debian.
- [Sparky Stereo OS](https://github.com/Sparky-OS/sparky-stereo-os) is the stereo 3D edition of SparkyLinux: SparkyOS, powered by Debian.

The `sparky/stereo3d-6.11` branch holds the version the distribution builds.
It sits on the `stereo3d-6.11` branch, which holds the changes offered to Qt.
The licences are unchanged; see [LICENSES](LICENSES).
Qt is a registered trademark of The Qt Company Ltd. and its subsidiaries.

Sparky Stereo OS, Daniel Ramos's edition of SparkyLinux (by Paweł "pavroo" Pijanowski).
