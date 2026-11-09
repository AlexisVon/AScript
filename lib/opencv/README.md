# Vendored OpenCV

`links/cv` links OpenCV **statically** into `bin/links/cv.so` (Linux) or
`bin/Cv.dll` (Windows), so no OpenCV runtime files are deployed. This directory
holds the pinned install trees; only this README is tracked by git.

## Pinned version

OpenCV 4.14.0, tag commit `0654a42e19215ef25b1d367d822f3c630447e7c7`, modules
`core`, `imgproc`, `imgcodecs`, with bundled PNG/JPEG/TIFF/WebP/Zlib codecs.
Apache-2.0; bundled third-party notices are installed under
`share/licenses/opencv4/` (Linux) or `etc/licenses/` (Windows) and must ship
with any redistribution of `cv.so` / `Cv.dll`.

## Expected tree

```
lib/opencv/
  include/opencv4/opencv2/...
  lib/libopencv_core.a
  lib/libopencv_imgproc.a
  lib/libopencv_imgcodecs.a
  lib/opencv4/3rdparty/lib*.a   # bundled codecs
  lib/cmake/opencv4/OpenCVConfig.cmake
  share/licenses/opencv4/       # third-party notices
```

`links/cv/CMakeLists.txt` requires `lib/cmake/opencv4/OpenCVConfig.cmake` and
stops the configure with an error when the tree is missing or partial.

The Windows install tree is separate (`lib/opencv/win-x64/`): its archives live
under `x64/vc18/staticlib/` as `.lib` and come in Release and Debug (`d`) sets,
so they cannot share `lib/` with the Linux `.a` tree:

```
lib/opencv/win-x64/
  include/opencv2/...                          # include root is include/, not include/opencv4/
  x64/vc18/staticlib/opencv_core4140.lib       # + opencv_core4140d.lib (Debug)
  x64/vc18/staticlib/*.lib                     # imgproc, imgcodecs, bundled codecs
  OpenCVConfig.cmake
  etc/licenses/                                # third-party notices
```

`vc18` is the VS 2026 toolset directory; a VS 2022 build installs under `vc17`.

`msvc/Cv/Cv.vcxproj` reads this tree; `$(SolutionDir)..\lib\opencv\win-x64`.

## Rebuild (Linux)

The static archives must be position independent: without
`CMAKE_POSITION_INDEPENDENT_CODE=ON` they cannot be linked into `cv.so`.

```bash
git clone --depth 1 --branch 4.14.0 https://github.com/opencv/opencv.git /tmp/opencv-src

cmake -S /tmp/opencv-src -B /tmp/opencv-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$PWD/lib/opencv \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_LIST=core,imgproc,imgcodecs \
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
  -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_PYTHON=OFF \
  -DWITH_IPP=OFF -DWITH_OPENCL=OFF -DWITH_FFMPEG=OFF -DWITH_GTK=OFF \
  -DWITH_QT=OFF -DWITH_OPENMP=OFF -DWITH_OPENEXR=OFF -DWITH_LAPACK=OFF \
  -DWITH_EIGEN=OFF -DWITH_ITT=OFF -DWITH_V4L=OFF -DWITH_1394=OFF \
  -DWITH_GPHOTO2=OFF -DWITH_OPENNI=OFF -DWITH_OPENNI2=OFF -DWITH_GDAL=OFF \
  -DWITH_GDCM=OFF -DWITH_QUIRC=OFF -DWITH_ADE=OFF -DWITH_AVIF=OFF \
  -DWITH_OPENJPEG=OFF -DWITH_VA=OFF -DWITH_VA_INTEL=OFF -DBUILD_PROTOBUF=OFF \
  -DBUILD_PNG=ON -DBUILD_JPEG=ON -DBUILD_TIFF=ON -DBUILD_WEBP=ON -DBUILD_ZLIB=ON \
  -DOPENCV_ENABLE_NONFREE=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON

cmake --build /tmp/opencv-build -j7
cmake --install /tmp/opencv-build
```

## Rebuild (Windows / MSVC)

The MSVC project links the Debug and Release archive sets, so build and install
both configurations. `BUILD_WITH_STATIC_CRT=OFF` is required: OpenCV otherwise
builds with `/MT`, which clashes with the `/MD`/`/MDd` used by the link
projects. Install to `lib/opencv/win-x64/`.

```powershell
cmake -S opencv -B opencv-build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_INSTALL_PREFIX=<repo>/lib/opencv/win-x64 `
  -DBUILD_WITH_STATIC_CRT=OFF `
  -DBUILD_SHARED_LIBS=OFF -DBUILD_LIST=core,imgproc,imgcodecs `
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF `
  -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_PYTHON=OFF `
  -DWITH_IPP=OFF -DWITH_OPENCL=OFF -DWITH_FFMPEG=OFF -DWITH_QT=OFF `
  -DWITH_OPENMP=OFF -DWITH_OPENEXR=OFF -DWITH_LAPACK=OFF -DWITH_EIGEN=OFF `
  -DWITH_ITT=OFF -DWITH_GDAL=OFF -DWITH_GDCM=OFF -DWITH_ADE=OFF `
  -DWITH_OPENJPEG=OFF -DWITH_VA=OFF -DWITH_VA_INTEL=OFF -DBUILD_PROTOBUF=OFF `
  -DBUILD_PNG=ON -DBUILD_JPEG=ON -DBUILD_TIFF=ON -DBUILD_WEBP=ON -DBUILD_ZLIB=ON `
  -DOPENCV_ENABLE_NONFREE=OFF

cmake --build opencv-build --config Debug
cmake --build opencv-build --config Release
cmake --install opencv-build --config Debug
cmake --install opencv-build --config Release
```

Debug archives carry a `d` suffix (`opencv_core4140d.lib`); `msvc/Cv/Cv.vcxproj`
selects the matching set per configuration.
