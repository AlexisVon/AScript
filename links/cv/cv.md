# cv Link

```ascript
link "bin/links/cv.so" as cv;
```

补全元数据在 `cv.json`（与 `cv.so` 同目录），GUI 按 `link ... as alias` 的别名提示 `alias.`。

OpenCV 4.14.0 静态编入 `cv.so`。图像是 `cv::Mat` 的不透明句柄；脚本变量之间的句柄拷贝共享同一块像素，绘制类函数原地修改，对副本同样可见。所有失败（读不到文件、句柄类型不符、空图、OpenCV 断言）一律抛异常，不返回 null。

- 颜色参数统一为 `0xRRGGBB`，内部转 BGR；单通道图像按亮度取值。
- 类型串：`8u`、`8s`、`16u`、`16s`、`32s`、`32f`、`64f`，可带 `cN`（如 `c3`、`c10`）。
- 纯算子返回新句柄；`line` / `rect` / `circle` / `ellipse` / `text` 原地修改并返回同一句柄。
- 同一个 Mat 被多个脚本线程并发写由调用方负责，库不加锁（OpenCV 语义）。

## IO

### `cv.imread(path, [mode="color"]) → handle`
`mode`：`color` | `gray` | `unchanged`。

### `cv.imwrite(path, img, [params]) → bool`
`params` 可选 map：`jpeg_quality`、`png_compression`、`webp_quality`、`tiff_compression`。

### `cv.imdecode(data, [mode="color"]) → handle`
### `cv.imencode(img, ext, [params]) → bytes`
`ext` 可写 `"png"` 或 `".png"`，支持 OpenCV 自带的 PNG/JPEG/WebP/TIFF/BMP 编码器。
### `cv.create(width, height, type, [color]) → handle`
### `cv.clone(img) → handle`
### `cv.info(img) → map`
返回 `{width, height, channels, depth, mat_type}`（`type` 是语言内置函数名，不能作字段名）。
### `cv.version() → string`

## 几何

### `cv.resize(img, width, height, [interp="linear"]) → handle`
`interp`：`nearest` | `linear` | `cubic` | `area` | `lanczos`。
### `cv.crop(img, x, y, width, height) → handle`
裁剪并复制，不共享原图。
### `cv.rotate(img, angle) → handle`
`angle`：`90` / `180` / `270`（负值等价）。
### `cv.flip(img, axis) → handle`
`axis`：`"x"`（上下翻转）| `"y"`（左右翻转）| `"both"`，也接受 `0` / `1` / `-1`。
### `cv.transpose(img) → handle`

## 色彩与阈值

### `cv.cvt_color(img, code) → handle`
`code`：`bgr2gray`、`rgb2gray`、`gray2bgr`、`gray2rgb`、`bgr2rgb`、`rgb2bgr`、`bgra2bgr`、`bgr2bgra`、`bgr2hsv`、`hsv2bgr`、`bgr2hls`、`hls2bgr`、`bgr2yuv`、`yuv2bgr`、`bgr2lab`、`lab2bgr`、`bgr2luv`、`luv2bgr`。
### `cv.threshold(img, thresh, maxval, [type="binary"]) → handle`
`type`：`binary`、`binary_inv`、`trunc`、`tozero`、`tozero_inv`、`otsu`、`otsu_inv`、`triangle`、`triangle_inv`。Otsu/triangle 计算出的阈值不回传，只返回图像。
### `cv.split(img) → vec<handle>`
### `cv.merge(images) → handle`
### `cv.convert(img, type, [scale=1], [offset=0]) → handle`
只改深度；`type` 的通道数必须与输入一致（要改通道用 `cvt_color` 或 `split`/`merge`）。

## 滤波

### `cv.blur(img, ksize) → handle`
### `cv.gaussian_blur(img, ksize, [sigma=0]) → handle`
`ksize` 为 0 时必须给正 `sigma`。
### `cv.median_blur(img, ksize) → handle`
`ksize` 为奇数且 ≥ 3。
### `cv.bilateral(img, d, sigma_color, sigma_space) → handle`
### `cv.sobel(img, dx, dy, [ksize=3]) → handle`
输出 `16s`，需要显示时用 `cv.convert` 转 `8u`。
### `cv.laplacian(img, [ksize=1]) → handle`
输出 `16s`。
### `cv.canny(img, low, high, [aperture=3]) → handle`
输入为 8 位单通道。

## 形态学

### `cv.erode(img, ksize, [iterations=1]) → handle`
### `cv.dilate(img, ksize, [iterations=1]) → handle`
### `cv.morphology(img, op, ksize, [iterations=1]) → handle`
`op`：`open`、`close`、`gradient`、`tophat`、`blackhat`。

## 绘图（原地）

### `cv.line(img, x1, y1, x2, y2, color, [thickness=1], [line_type=8]) → handle`
### `cv.rect(img, x, y, width, height, color, [thickness=1], [line_type=8]) → handle`
`thickness < 0` 填充。
### `cv.circle(img, cx, cy, radius, color, [thickness=1], [line_type=8]) → handle`
### `cv.ellipse(img, cx, cy, ax, ay, angle, start, end, color, [thickness=1], [line_type=8]) → handle`
角度单位为度。
### `cv.text(img, text, x, y, scale, color, [thickness=1], [font="simplex"], [line_type=8]) → handle`
`font`：`simplex`、`plain`、`duplex`、`complex`、`triplex`、`complex_small`、`script_simplex`、`script_complex`。Hershey 字体只支持 ASCII，非 ASCII 字符会显示为 `?`。

## 算术

### `cv.add(a, b) → handle`
### `cv.subtract(a, b) → handle`
### `cv.absdiff(a, b) → handle`
### `cv.bitwise_and(a, b) → handle`
### `cv.bitwise_or(a, b) → handle`
### `cv.bitwise_xor(a, b) → handle`
### `cv.bitwise_not(img) → handle`
### `cv.blend(a, b, alpha) → handle`
`out = a * alpha + b * (1 - alpha)`，`alpha` 在 0..1。

## 示例

```ascript
link "bin/links/cv.so" as cv;

var img = cv.imread("in.jpg", "color");
var small = cv.resize(img, 640, 480, "area");
var gray = cv.cvt_color(small, "bgr2gray");
var edges = cv.canny(gray, 80, 160);

cv.rect(edges, 10, 10, 120, 60, 0xFFFFFF, 2);
cv.text(edges, "edges", 16, 40, 0.8, 0xFF0000, 2);

var png = cv.imencode(edges, "png", map{"png_compression": 6});
cv.imwrite("out.png", edges);
var info = cv.info(edges);
$io_print("%1x%2, %3 bytes\n", info.width, info.height, $bytes_size(png));
```

## 调试视图

调试器暂停时，变量树里的图像句柄可打开“视图”。cv 为 `cv::Mat` 注册类型名与一行摘要（`宽x高 c通道 深度`），查看时返回全尺寸 BMP：

- 8U 的 1/3 通道直接输出；4 通道丢弃 alpha。
- 16U、整型、浮点先归一化到 8U，视图标注“显示已归一化”。
- 2 通道或超过 4 通道不提供视图，调试器显示打开失败。
