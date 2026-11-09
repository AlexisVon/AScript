// cv link — OpenCV image operators over anyptr<cv::Mat> handles
#include "ascript.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

    using alx::script::error_type;
    using alx::script::fwrap;
    using alx::script::script_exception;

    [[noreturn]] void fail(const char* _fn, const std::string& _msg, error_type _type = error_type::RuntimeError) {
        throw script_exception(_type, std::string("cv.") + _fn + ": " + _msg);
    }

    template <typename F>
    void guard(const char* _fn, F&& _body) {
        try {
            _body();
        } catch (const script_exception&) {
            throw;
        } catch (const cv::Exception& _e) {
            fail(_fn, _e.what());
        }
    }

    // --- arguments ---
    alx::variant& req(fwrap& _fw, size_t _i, const char* _fn, const char* _name) {
        if (_i >= _fw.size())
            fail(_fn, std::string("missing argument '") + _name + "'", error_type::ArgError);
        return _fw[_i];
    }

    std::int64_t integer(fwrap& _fw, size_t _i, const char* _fn, const char* _name) {
        const alx::variant& v = req(_fw, _i, _fn, _name);
        if (!v.is<alx::int_64>())
            fail(_fn, std::string("'") + _name + "' expected int", error_type::TypeError);
        return v.to<alx::int_64>();
    }

    double number(fwrap& _fw, size_t _i, const char* _fn, const char* _name) {
        const alx::variant& v = req(_fw, _i, _fn, _name);
        if (v.is<double>()) return v.to<double>();
        if (v.is<alx::int_64>()) return static_cast<double>(v.to<alx::int_64>());
        fail(_fn, std::string("'") + _name + "' expected number", error_type::TypeError);
    }

    std::string text(fwrap& _fw, size_t _i, const char* _fn, const char* _name) {
        const alx::variant& v = req(_fw, _i, _fn, _name);
        if (!v.is<std::string>())
            fail(_fn, std::string("'") + _name + "' expected string", error_type::TypeError);
        return v.to<std::string>();
    }

    const alx::bytes& bytes_arg(fwrap& _fw, size_t _i, const char* _fn, const char* _name) {
        const alx::variant& v = req(_fw, _i, _fn, _name);
        if (!v.is<alx::bytes>())
            fail(_fn, std::string("'") + _name + "' expected bytes", error_type::TypeError);
        return v.to<alx::bytes>();
    }

    std::int64_t integer_or(fwrap& _fw, size_t _i, const char* _fn, const char* _name, std::int64_t _def) {
        return _i < _fw.size() ? integer(_fw, _i, _fn, _name) : _def;
    }

    int dimension(fwrap& _fw, size_t _i, const char* _fn, const char* _name) {
        std::int64_t v = integer(_fw, _i, _fn, _name);
        if (v <= 0 || v > INT_MAX)
            fail(_fn, std::string("'") + _name + "' out of range", error_type::ArgError);
        return static_cast<int>(v);
    }

    double number_or(fwrap& _fw, size_t _i, const char* _fn, const char* _name, double _def) {
        return _i < _fw.size() ? number(_fw, _i, _fn, _name) : _def;
    }

    std::string text_or(fwrap& _fw, size_t _i, const char* _fn, const char* _name, const char* _def) {
        return _i < _fw.size() ? text(_fw, _i, _fn, _name) : std::string(_def);
    }

    // --- handles ---
    cv::Mat* image(fwrap& _fw, size_t _i, const char* _fn, const char* _name) {
        alx::variant& v = req(_fw, _i, _fn, _name);
        if (!v.is<alx::anyptr>())
            fail(_fn, std::string("'") + _name + "' expected image handle", error_type::TypeError);
        cv::Mat* m = alx::anyptr_ex<cv::Mat>::as(v.to<alx::anyptr>());
        if (!m)
            fail(_fn, std::string("'") + _name + "' is not a cv image handle", error_type::TypeError);
        if (m->empty())
            fail(_fn, std::string("'") + _name + "' is an empty image");
        return m;
    }

    alx::variant wrap(cv::Mat&& _m) {
        return alx::variant(alx::anyptr_ex<cv::Mat>::make_ref(new cv::Mat(std::move(_m))));
    }

    // --- enum parsing ---
    int read_flags(const std::string& _mode, const char* _fn) {
        if (_mode == "color") return cv::IMREAD_COLOR;
        if (_mode == "gray") return cv::IMREAD_GRAYSCALE;
        if (_mode == "unchanged") return cv::IMREAD_UNCHANGED;
        fail(_fn, "mode must be color|gray|unchanged", error_type::ArgError);
    }

    int depth_of(const std::string& _s, const char* _fn) {
        if (_s == "8u") return CV_8U;
        if (_s == "8s") return CV_8S;
        if (_s == "16u") return CV_16U;
        if (_s == "16s") return CV_16S;
        if (_s == "32s") return CV_32S;
        if (_s == "32f") return CV_32F;
        if (_s == "64f") return CV_64F;
        fail(_fn, "unknown depth '" + _s + "'", error_type::ArgError);
    }

    int type_of(const std::string& _s, const char* _fn) {
        size_t c = _s.find('c');
        int depth = depth_of(c == std::string::npos ? _s : _s.substr(0, c), _fn);
        int channels = 1;
        if (c != std::string::npos) {
            int parsed = 0;
            for (size_t i = c + 1; i < _s.size(); i++) {
                if (_s[i] < '0' || _s[i] > '9' || parsed > CV_CN_MAX)
                    fail(_fn, "bad type '" + _s + "' (use 8u, 8uc3, 32fc1, ...)", error_type::ArgError);
                parsed = parsed * 10 + (_s[i] - '0');
            }
            if (parsed < 1 || parsed > CV_CN_MAX)
                fail(_fn, "bad type '" + _s + "' (use 8u, 8uc3, 32fc1, ...)", error_type::ArgError);
            channels = parsed;
        }
        return CV_MAKETYPE(depth, channels);
    }

    const char* depth_name(int _depth) {
        switch (_depth) {
        case CV_8U: return "8u";
        case CV_8S: return "8s";
        case CV_16U: return "16u";
        case CV_16S: return "16s";
        case CV_32S: return "32s";
        case CV_32F: return "32f";
        case CV_64F: return "64f";
        default: return "?";
        }
    }

    std::string type_name(int _type) {
        return std::string(depth_name(CV_MAT_DEPTH(_type))) + "c" + std::to_string(CV_MAT_CN(_type));
    }

    cv::Scalar color_of(std::int64_t _rgb, int _channels, const char* _fn, const char* _name) {
        if (_rgb < 0 || _rgb > 0XFFFFFF)
            fail(_fn, std::string("'") + _name + "' must be 0xRRGGBB", error_type::ArgError);
        auto r = (_rgb >> 16) & 0XFF, g = (_rgb >> 8) & 0XFF, b = _rgb & 0XFF;
        if (_channels == 1) return cv::Scalar(std::lround(0.299 * r + 0.587 * g + 0.114 * b));
        if (_channels == 4) return cv::Scalar(b, g, r, 255);
        return cv::Scalar(b, g, r);
    }

    int interp_of(const std::string& _s, const char* _fn) {
        if (_s == "nearest") return cv::INTER_NEAREST;
        if (_s == "linear") return cv::INTER_LINEAR;
        if (_s == "cubic") return cv::INTER_CUBIC;
        if (_s == "area") return cv::INTER_AREA;
        if (_s == "lanczos") return cv::INTER_LANCZOS4;
        fail(_fn, "unknown interpolation '" + _s + "'", error_type::ArgError);
    }

    int cvt_of(const std::string& _s, const char* _fn) {
        if (_s == "bgr2gray") return cv::COLOR_BGR2GRAY;
        if (_s == "rgb2gray") return cv::COLOR_RGB2GRAY;
        if (_s == "gray2bgr") return cv::COLOR_GRAY2BGR;
        if (_s == "gray2rgb") return cv::COLOR_GRAY2RGB;
        if (_s == "bgr2rgb") return cv::COLOR_BGR2RGB;
        if (_s == "rgb2bgr") return cv::COLOR_RGB2BGR;
        if (_s == "bgra2bgr") return cv::COLOR_BGRA2BGR;
        if (_s == "bgr2bgra") return cv::COLOR_BGR2BGRA;
        if (_s == "bgr2hsv") return cv::COLOR_BGR2HSV;
        if (_s == "hsv2bgr") return cv::COLOR_HSV2BGR;
        if (_s == "bgr2hls") return cv::COLOR_BGR2HLS;
        if (_s == "hls2bgr") return cv::COLOR_HLS2BGR;
        if (_s == "bgr2yuv") return cv::COLOR_BGR2YUV;
        if (_s == "yuv2bgr") return cv::COLOR_YUV2BGR;
        if (_s == "bgr2lab") return cv::COLOR_BGR2Lab;
        if (_s == "lab2bgr") return cv::COLOR_Lab2BGR;
        if (_s == "bgr2luv") return cv::COLOR_BGR2Luv;
        if (_s == "luv2bgr") return cv::COLOR_Luv2BGR;
        fail(_fn, "unknown color code '" + _s + "'", error_type::ArgError);
    }

    int thresh_of(const std::string& _s, const char* _fn) {
        if (_s == "binary") return cv::THRESH_BINARY;
        if (_s == "binary_inv") return cv::THRESH_BINARY_INV;
        if (_s == "trunc") return cv::THRESH_TRUNC;
        if (_s == "tozero") return cv::THRESH_TOZERO;
        if (_s == "tozero_inv") return cv::THRESH_TOZERO_INV;
        if (_s == "otsu") return cv::THRESH_BINARY | cv::THRESH_OTSU;
        if (_s == "otsu_inv") return cv::THRESH_BINARY_INV | cv::THRESH_OTSU;
        if (_s == "triangle") return cv::THRESH_BINARY | cv::THRESH_TRIANGLE;
        if (_s == "triangle_inv") return cv::THRESH_BINARY_INV | cv::THRESH_TRIANGLE;
        fail(_fn, "unknown threshold type '" + _s + "'", error_type::ArgError);
    }

    int morph_of(const std::string& _s, const char* _fn) {
        if (_s == "open") return cv::MORPH_OPEN;
        if (_s == "close") return cv::MORPH_CLOSE;
        if (_s == "gradient") return cv::MORPH_GRADIENT;
        if (_s == "tophat") return cv::MORPH_TOPHAT;
        if (_s == "blackhat") return cv::MORPH_BLACKHAT;
        fail(_fn, "unknown morphology op '" + _s + "'", error_type::ArgError);
    }

    int font_of(const std::string& _s, const char* _fn) {
        if (_s == "simplex") return cv::FONT_HERSHEY_SIMPLEX;
        if (_s == "plain") return cv::FONT_HERSHEY_PLAIN;
        if (_s == "duplex") return cv::FONT_HERSHEY_DUPLEX;
        if (_s == "complex") return cv::FONT_HERSHEY_COMPLEX;
        if (_s == "triplex") return cv::FONT_HERSHEY_TRIPLEX;
        if (_s == "complex_small") return cv::FONT_HERSHEY_COMPLEX_SMALL;
        if (_s == "script_simplex") return cv::FONT_HERSHEY_SCRIPT_SIMPLEX;
        if (_s == "script_complex") return cv::FONT_HERSHEY_SCRIPT_COMPLEX;
        fail(_fn, "unknown font '" + _s + "'", error_type::ArgError);
    }

    std::vector<int> write_params(fwrap& _fw, size_t _i, const char* _fn) {
        std::vector<int> params;
        if (_fw.size() <= _i) return params;
        if (!_fw[_i].is<alx::varmap>())
            fail(_fn, "params expected map", error_type::TypeError);
        const alx::varmap& opts = _fw[_i].to<alx::varmap>();
        auto add = [&](const char* _key, int _id) {
            if (!opts.contain(_key)) return;
            const alx::variant& v = opts.value(_key);
            if (!v.is<alx::int_64>())
                fail(_fn, std::string("params.") + _key + " expected int", error_type::TypeError);
            params.push_back(_id);
            params.push_back(static_cast<int>(v.to<alx::int_64>()));
        };
        add("jpeg_quality", cv::IMWRITE_JPEG_QUALITY);
        add("png_compression", cv::IMWRITE_PNG_COMPRESSION);
        add("webp_quality", cv::IMWRITE_WEBP_QUALITY);
        add("tiff_compression", cv::IMWRITE_TIFF_COMPRESSION);
        return params;
    }

    cv::Mat kernel_of(std::int64_t _ksize, const char* _fn) {
        if (_ksize <= 0)
            fail(_fn, "ksize must be positive", error_type::ArgError);
        return cv::getStructuringElement(cv::MORPH_RECT, cv::Size(static_cast<int>(_ksize), static_cast<int>(_ksize)));
    }

    // --- IO ---
    void fn_imread(fwrap& _fw) {
        guard("imread", [&] {
            std::string path = text(_fw, 0, "imread", "path");
            int flags = read_flags(text_or(_fw, 1, "imread", "mode", "color"), "imread");
            cv::Mat m = cv::imread(path, flags);
            if (m.empty()) fail("imread", "cannot read '" + path + "'");
            _fw.freturn(wrap(std::move(m)));
        });
    }

    void fn_imwrite(fwrap& _fw) {
        guard("imwrite", [&] {
            std::string path = text(_fw, 0, "imwrite", "path");
            cv::Mat* m = image(_fw, 1, "imwrite", "img");
            if (!cv::imwrite(path, *m, write_params(_fw, 2, "imwrite")))
                fail("imwrite", "cannot write '" + path + "'");
            _fw.freturn(alx::variant(true));
        });
    }

    void fn_imdecode(fwrap& _fw) {
        guard("imdecode", [&] {
            const alx::bytes& b = bytes_arg(_fw, 0, "imdecode", "data");
            if (b.size() > INT_MAX) fail("imdecode", "data too large", error_type::ArgError);
            int flags = read_flags(text_or(_fw, 1, "imdecode", "mode", "color"), "imdecode");
            cv::Mat raw(1, static_cast<int>(b.size()), CV_8U, const_cast<alx::uint_8*>(b.data()));
            cv::Mat m = cv::imdecode(raw, flags);
            if (m.empty()) fail("imdecode", "cannot decode image data");
            _fw.freturn(wrap(std::move(m)));
        });
    }

    void fn_imencode(fwrap& _fw) {
        guard("imencode", [&] {
            cv::Mat* m = image(_fw, 0, "imencode", "img");
            std::string ext = text(_fw, 1, "imencode", "ext");
            if (ext.empty()) fail("imencode", "'ext' must not be empty", error_type::ArgError);
            if (ext.front() != '.') ext.insert(ext.begin(), '.');
            std::vector<unsigned char> buf;
            if (!cv::imencode(ext, *m, buf, write_params(_fw, 2, "imencode")))
                fail("imencode", "cannot encode as '" + ext + "'");
            _fw.freturn(alx::variant(alx::bytes(buf.data(), buf.size())));
        });
    }

    void fn_create(fwrap& _fw) {
        guard("create", [&] {
            int w = dimension(_fw, 0, "create", "width");
            int h = dimension(_fw, 1, "create", "height");
            int type = type_of(text(_fw, 2, "create", "type"), "create");
            cv::Scalar fill;
            if (_fw.size() > 3)
                fill = color_of(integer(_fw, 3, "create", "color"), CV_MAT_CN(type), "create", "color");
            cv::Mat m(h, w, type, fill);
            _fw.freturn(wrap(std::move(m)));
        });
    }

    void fn_clone(fwrap& _fw) {
        guard("clone", [&] {
            cv::Mat* m = image(_fw, 0, "clone", "img");
            _fw.freturn(wrap(m->clone()));
        });
    }

    void fn_info(fwrap& _fw) {
        guard("info", [&] {
            cv::Mat* m = image(_fw, 0, "info", "img");
            alx::varmap r;
            r["width"] = alx::variant(static_cast<alx::int_64>(m->cols));
            r["height"] = alx::variant(static_cast<alx::int_64>(m->rows));
            r["channels"] = alx::variant(static_cast<alx::int_64>(m->channels()));
            r["depth"] = alx::variant(std::string(depth_name(m->depth())));
            r["mat_type"] = alx::variant(type_name(m->type()));
            _fw.freturn(alx::variant(std::move(r)));
        });
    }

    void fn_version(fwrap& _fw) {
        guard("version", [&] { _fw.freturn(alx::variant(std::string(CV_VERSION))); });
    }

    // --- geometry ---
    void fn_resize(fwrap& _fw) {
        guard("resize", [&] {
            cv::Mat* m = image(_fw, 0, "resize", "img");
            int w = dimension(_fw, 1, "resize", "width");
            int h = dimension(_fw, 2, "resize", "height");
            int interp = interp_of(text_or(_fw, 3, "resize", "interp", "linear"), "resize");
            cv::Mat out;
            cv::resize(*m, out, cv::Size(w, h), 0, 0, interp);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_crop(fwrap& _fw) {
        guard("crop", [&] {
            cv::Mat* m = image(_fw, 0, "crop", "img");
            std::int64_t x = integer(_fw, 1, "crop", "x");
            std::int64_t y = integer(_fw, 2, "crop", "y");
            std::int64_t w = integer(_fw, 3, "crop", "width");
            std::int64_t h = integer(_fw, 4, "crop", "height");
            if (x < 0 || y < 0 || w <= 0 || h <= 0 || x > m->cols - w || y > m->rows - h)
                fail("crop", "rectangle outside the image", error_type::ArgError);
            cv::Mat out = (*m)(cv::Rect(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h))).clone();
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_rotate(fwrap& _fw) {
        guard("rotate", [&] {
            cv::Mat* m = image(_fw, 0, "rotate", "img");
            std::int64_t angle = integer(_fw, 1, "rotate", "angle");
            int code;
            switch (angle) {
            case 90:
            case -270: code = cv::ROTATE_90_CLOCKWISE; break;
            case 180:
            case -180: code = cv::ROTATE_180; break;
            case 270:
            case -90: code = cv::ROTATE_90_COUNTERCLOCKWISE; break;
            default: fail("rotate", "angle must be 90, 180 or 270", error_type::ArgError);
            }
            cv::Mat out;
            cv::rotate(*m, out, code);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_flip(fwrap& _fw) {
        guard("flip", [&] {
            cv::Mat* m = image(_fw, 0, "flip", "img");
            alx::variant& axis_arg = req(_fw, 1, "flip", "axis");
            std::int64_t code;
            if (axis_arg.is<std::string>()) {
                std::string axis = axis_arg.to<std::string>();
                if (axis == "x") code = 0;
                else if (axis == "y") code = 1;
                else if (axis == "both") code = -1;
                else fail("flip", "axis must be x|y|both", error_type::ArgError);
            } else {
                code = integer(_fw, 1, "flip", "axis");
                if (code != 0 && code != 1 && code != -1)
                    fail("flip", "axis must be 0|1|-1", error_type::ArgError);
            }
            cv::Mat out;
            cv::flip(*m, out, static_cast<int>(code));
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_transpose(fwrap& _fw) {
        guard("transpose", [&] {
            cv::Mat* m = image(_fw, 0, "transpose", "img");
            cv::Mat out;
            cv::transpose(*m, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    // --- color and threshold ---
    void fn_cvt_color(fwrap& _fw) {
        guard("cvt_color", [&] {
            cv::Mat* m = image(_fw, 0, "cvt_color", "img");
            int code = cvt_of(text(_fw, 1, "cvt_color", "code"), "cvt_color");
            cv::Mat out;
            cv::cvtColor(*m, out, code);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_threshold(fwrap& _fw) {
        guard("threshold", [&] {
            cv::Mat* m = image(_fw, 0, "threshold", "img");
            double thresh = number(_fw, 1, "threshold", "thresh");
            double maxval = number(_fw, 2, "threshold", "maxval");
            int type = thresh_of(text_or(_fw, 3, "threshold", "type", "binary"), "threshold");
            cv::Mat out;
            cv::threshold(*m, out, thresh, maxval, type);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_split(fwrap& _fw) {
        guard("split", [&] {
            cv::Mat* m = image(_fw, 0, "split", "img");
            std::vector<cv::Mat> ch;
            cv::split(*m, ch);
            alx::varvec out;
            out.reserve(ch.size());
            for (auto& c : ch) out.push_back(wrap(std::move(c)));
            _fw.freturn(alx::variant(std::move(out)));
        });
    }

    void fn_merge(fwrap& _fw) {
        guard("merge", [&] {
            alx::variant& v = req(_fw, 0, "merge", "images");
            if (!v.is<alx::varvec>())
                fail("merge", "'images' expected vec", error_type::TypeError);
            const alx::varvec& list = v.to<alx::varvec>();
            if (list.empty()) fail("merge", "empty list", error_type::ArgError);
            std::vector<cv::Mat> mats;
            mats.reserve(list.size());
            for (size_t i = 0; i < list.size(); i++) {
                const alx::variant& e = list[i];
                if (!e.is<alx::anyptr>())
                    fail("merge", "element " + std::to_string(i) + " is not an image handle", error_type::TypeError);
                cv::Mat* p = alx::anyptr_ex<cv::Mat>::as(e.to<alx::anyptr>());
                if (!p || p->empty())
                    fail("merge", "element " + std::to_string(i) + " is not a cv image handle", error_type::TypeError);
                mats.push_back(*p);
            }
            cv::Mat out;
            cv::merge(mats, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_convert(fwrap& _fw) {
        guard("convert", [&] {
            cv::Mat* m = image(_fw, 0, "convert", "img");
            int type = type_of(text(_fw, 1, "convert", "type"), "convert");
            if (CV_MAT_CN(type) != m->channels())
                fail("convert", "channel count must match the input; use cvt_color or split/merge",
                     error_type::ArgError);
            double scale = number_or(_fw, 2, "convert", "scale", 1.0);
            double offset = number_or(_fw, 3, "convert", "offset", 0.0);
            cv::Mat out;
            m->convertTo(out, type, scale, offset);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    // --- filters ---
    void fn_blur(fwrap& _fw) {
        guard("blur", [&] {
            cv::Mat* m = image(_fw, 0, "blur", "img");
            std::int64_t k = integer(_fw, 1, "blur", "ksize");
            if (k <= 0) fail("blur", "ksize must be positive", error_type::ArgError);
            cv::Mat out;
            cv::blur(*m, out, cv::Size(static_cast<int>(k), static_cast<int>(k)));
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_gaussian_blur(fwrap& _fw) {
        guard("gaussian_blur", [&] {
            cv::Mat* m = image(_fw, 0, "gaussian_blur", "img");
            std::int64_t k = integer(_fw, 1, "gaussian_blur", "ksize");
            double sigma = number_or(_fw, 2, "gaussian_blur", "sigma", 0.0);
            if (k == 0 && sigma <= 0)
                fail("gaussian_blur", "ksize 0 requires sigma > 0", error_type::ArgError);
            if (k != 0 && (k < 1 || k % 2 == 0))
                fail("gaussian_blur", "ksize must be odd", error_type::ArgError);
            cv::Mat out;
            cv::GaussianBlur(*m, out, cv::Size(static_cast<int>(k), static_cast<int>(k)), sigma);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_median_blur(fwrap& _fw) {
        guard("median_blur", [&] {
            cv::Mat* m = image(_fw, 0, "median_blur", "img");
            std::int64_t k = integer(_fw, 1, "median_blur", "ksize");
            if (k < 3 || k % 2 == 0)
                fail("median_blur", "ksize must be an odd number >= 3", error_type::ArgError);
            cv::Mat out;
            cv::medianBlur(*m, out, static_cast<int>(k));
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_bilateral(fwrap& _fw) {
        guard("bilateral", [&] {
            cv::Mat* m = image(_fw, 0, "bilateral", "img");
            int d = static_cast<int>(integer(_fw, 1, "bilateral", "d"));
            double sigma_color = number(_fw, 2, "bilateral", "sigma_color");
            double sigma_space = number(_fw, 3, "bilateral", "sigma_space");
            cv::Mat out;
            cv::bilateralFilter(*m, out, d, sigma_color, sigma_space);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_sobel(fwrap& _fw) {
        guard("sobel", [&] {
            cv::Mat* m = image(_fw, 0, "sobel", "img");
            int dx = static_cast<int>(integer(_fw, 1, "sobel", "dx"));
            int dy = static_cast<int>(integer(_fw, 2, "sobel", "dy"));
            std::int64_t k = integer_or(_fw, 3, "sobel", "ksize", 3);
            if (dx < 0 || dy < 0 || dx + dy == 0)
                fail("sobel", "dx and dy must be non-negative and not both zero", error_type::ArgError);
            if (k != 1 && k != 3 && k != 5 && k != 7)
                fail("sobel", "ksize must be 1, 3, 5 or 7", error_type::ArgError);
            cv::Mat out;
            cv::Sobel(*m, out, CV_16S, dx, dy, static_cast<int>(k));
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_laplacian(fwrap& _fw) {
        guard("laplacian", [&] {
            cv::Mat* m = image(_fw, 0, "laplacian", "img");
            std::int64_t k = integer_or(_fw, 1, "laplacian", "ksize", 1);
            if (k != 1 && k != 3 && k != 5 && k != 7)
                fail("laplacian", "ksize must be 1, 3, 5 or 7", error_type::ArgError);
            cv::Mat out;
            cv::Laplacian(*m, out, CV_16S, static_cast<int>(k));
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_canny(fwrap& _fw) {
        guard("canny", [&] {
            cv::Mat* m = image(_fw, 0, "canny", "img");
            double low = number(_fw, 1, "canny", "low");
            double high = number(_fw, 2, "canny", "high");
            int aperture = static_cast<int>(integer_or(_fw, 3, "canny", "aperture", 3));
            cv::Mat out;
            cv::Canny(*m, out, low, high, aperture);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    // --- morphology ---
    void fn_erode(fwrap& _fw) {
        guard("erode", [&] {
            cv::Mat* m = image(_fw, 0, "erode", "img");
            cv::Mat kernel = kernel_of(integer(_fw, 1, "erode", "ksize"), "erode");
            int iterations = static_cast<int>(integer_or(_fw, 2, "erode", "iterations", 1));
            cv::Mat out;
            cv::erode(*m, out, kernel, cv::Point(-1, -1), iterations);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_dilate(fwrap& _fw) {
        guard("dilate", [&] {
            cv::Mat* m = image(_fw, 0, "dilate", "img");
            cv::Mat kernel = kernel_of(integer(_fw, 1, "dilate", "ksize"), "dilate");
            int iterations = static_cast<int>(integer_or(_fw, 2, "dilate", "iterations", 1));
            cv::Mat out;
            cv::dilate(*m, out, kernel, cv::Point(-1, -1), iterations);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_morphology(fwrap& _fw) {
        guard("morphology", [&] {
            cv::Mat* m = image(_fw, 0, "morphology", "img");
            int op = morph_of(text(_fw, 1, "morphology", "op"), "morphology");
            cv::Mat kernel = kernel_of(integer(_fw, 2, "morphology", "ksize"), "morphology");
            int iterations = static_cast<int>(integer_or(_fw, 3, "morphology", "iterations", 1));
            cv::Mat out;
            cv::morphologyEx(*m, out, op, kernel, cv::Point(-1, -1), iterations);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    // --- drawing: all mutate the handle in place and return it ---
    void fn_line(fwrap& _fw) {
        guard("line", [&] {
            cv::Mat* m = image(_fw, 0, "line", "img");
            int x1 = static_cast<int>(integer(_fw, 1, "line", "x1"));
            int y1 = static_cast<int>(integer(_fw, 2, "line", "y1"));
            int x2 = static_cast<int>(integer(_fw, 3, "line", "x2"));
            int y2 = static_cast<int>(integer(_fw, 4, "line", "y2"));
            cv::Scalar c = color_of(integer(_fw, 5, "line", "color"), m->channels(), "line", "color");
            int thickness = static_cast<int>(integer_or(_fw, 6, "line", "thickness", 1));
            int line_type = static_cast<int>(integer_or(_fw, 7, "line", "line_type", 8));
            cv::line(*m, cv::Point(x1, y1), cv::Point(x2, y2), c, thickness, line_type);
            _fw.freturn(_fw[0]);
        });
    }

    void fn_rect(fwrap& _fw) {
        guard("rect", [&] {
            cv::Mat* m = image(_fw, 0, "rect", "img");
            int x = static_cast<int>(integer(_fw, 1, "rect", "x"));
            int y = static_cast<int>(integer(_fw, 2, "rect", "y"));
            int w = static_cast<int>(integer(_fw, 3, "rect", "width"));
            int h = static_cast<int>(integer(_fw, 4, "rect", "height"));
            cv::Scalar c = color_of(integer(_fw, 5, "rect", "color"), m->channels(), "rect", "color");
            int thickness = static_cast<int>(integer_or(_fw, 6, "rect", "thickness", 1));
            int line_type = static_cast<int>(integer_or(_fw, 7, "rect", "line_type", 8));
            cv::rectangle(*m, cv::Rect(x, y, w, h), c, thickness, line_type);
            _fw.freturn(_fw[0]);
        });
    }

    void fn_circle(fwrap& _fw) {
        guard("circle", [&] {
            cv::Mat* m = image(_fw, 0, "circle", "img");
            int cx = static_cast<int>(integer(_fw, 1, "circle", "cx"));
            int cy = static_cast<int>(integer(_fw, 2, "circle", "cy"));
            int r = static_cast<int>(integer(_fw, 3, "circle", "radius"));
            cv::Scalar c = color_of(integer(_fw, 4, "circle", "color"), m->channels(), "circle", "color");
            int thickness = static_cast<int>(integer_or(_fw, 5, "circle", "thickness", 1));
            int line_type = static_cast<int>(integer_or(_fw, 6, "circle", "line_type", 8));
            cv::circle(*m, cv::Point(cx, cy), r, c, thickness, line_type);
            _fw.freturn(_fw[0]);
        });
    }

    void fn_ellipse(fwrap& _fw) {
        guard("ellipse", [&] {
            cv::Mat* m = image(_fw, 0, "ellipse", "img");
            int cx = static_cast<int>(integer(_fw, 1, "ellipse", "cx"));
            int cy = static_cast<int>(integer(_fw, 2, "ellipse", "cy"));
            int ax = static_cast<int>(integer(_fw, 3, "ellipse", "ax"));
            int ay = static_cast<int>(integer(_fw, 4, "ellipse", "ay"));
            double angle = number(_fw, 5, "ellipse", "angle");
            double start = number(_fw, 6, "ellipse", "start");
            double end = number(_fw, 7, "ellipse", "end");
            cv::Scalar c = color_of(integer(_fw, 8, "ellipse", "color"), m->channels(), "ellipse", "color");
            int thickness = static_cast<int>(integer_or(_fw, 9, "ellipse", "thickness", 1));
            int line_type = static_cast<int>(integer_or(_fw, 10, "ellipse", "line_type", 8));
            cv::ellipse(*m, cv::Point(cx, cy), cv::Size(ax, ay), angle, start, end, c, thickness, line_type);
            _fw.freturn(_fw[0]);
        });
    }

    void fn_text(fwrap& _fw) {
        guard("text", [&] {
            cv::Mat* m = image(_fw, 0, "text", "img");
            std::string s = text(_fw, 1, "text", "text");
            int x = static_cast<int>(integer(_fw, 2, "text", "x"));
            int y = static_cast<int>(integer(_fw, 3, "text", "y"));
            double scale = number(_fw, 4, "text", "scale");
            cv::Scalar c = color_of(integer(_fw, 5, "text", "color"), m->channels(), "text", "color");
            int thickness = static_cast<int>(integer_or(_fw, 6, "text", "thickness", 1));
            int font = font_of(text_or(_fw, 7, "text", "font", "simplex"), "text");
            int line_type = static_cast<int>(integer_or(_fw, 8, "text", "line_type", 8));
            cv::putText(*m, s, cv::Point(x, y), font, scale, c, thickness, line_type);
            _fw.freturn(_fw[0]);
        });
    }

    // --- arithmetic ---
    void fn_add(fwrap& _fw) {
        guard("add", [&] {
            cv::Mat* a = image(_fw, 0, "add", "a");
            cv::Mat* b = image(_fw, 1, "add", "b");
            cv::Mat out;
            cv::add(*a, *b, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_subtract(fwrap& _fw) {
        guard("subtract", [&] {
            cv::Mat* a = image(_fw, 0, "subtract", "a");
            cv::Mat* b = image(_fw, 1, "subtract", "b");
            cv::Mat out;
            cv::subtract(*a, *b, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_absdiff(fwrap& _fw) {
        guard("absdiff", [&] {
            cv::Mat* a = image(_fw, 0, "absdiff", "a");
            cv::Mat* b = image(_fw, 1, "absdiff", "b");
            cv::Mat out;
            cv::absdiff(*a, *b, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_bitwise_and(fwrap& _fw) {
        guard("bitwise_and", [&] {
            cv::Mat* a = image(_fw, 0, "bitwise_and", "a");
            cv::Mat* b = image(_fw, 1, "bitwise_and", "b");
            cv::Mat out;
            cv::bitwise_and(*a, *b, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_bitwise_or(fwrap& _fw) {
        guard("bitwise_or", [&] {
            cv::Mat* a = image(_fw, 0, "bitwise_or", "a");
            cv::Mat* b = image(_fw, 1, "bitwise_or", "b");
            cv::Mat out;
            cv::bitwise_or(*a, *b, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_bitwise_xor(fwrap& _fw) {
        guard("bitwise_xor", [&] {
            cv::Mat* a = image(_fw, 0, "bitwise_xor", "a");
            cv::Mat* b = image(_fw, 1, "bitwise_xor", "b");
            cv::Mat out;
            cv::bitwise_xor(*a, *b, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_bitwise_not(fwrap& _fw) {
        guard("bitwise_not", [&] {
            cv::Mat* a = image(_fw, 0, "bitwise_not", "img");
            cv::Mat out;
            cv::bitwise_not(*a, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    void fn_blend(fwrap& _fw) {
        guard("blend", [&] {
            cv::Mat* a = image(_fw, 0, "blend", "a");
            cv::Mat* b = image(_fw, 1, "blend", "b");
            double alpha = number(_fw, 2, "blend", "alpha");
            if (alpha < 0 || alpha > 1)
                fail("blend", "alpha must be in 0..1", error_type::ArgError);
            cv::Mat out;
            cv::addWeighted(*a, alpha, *b, 1.0 - alpha, 0, out);
            _fw.freturn(wrap(std::move(out)));
        });
    }

    // --- debug view: the engine asks for a one-line summary and a displayable frame ---
    std::string view_desc(const alx::anyptr& _h) {
        try {
            cv::Mat* m = alx::anyptr_ex<cv::Mat>::as(_h);
            if (!m || m->empty()) return std::string();
            return std::to_string(m->cols) + "x" + std::to_string(m->rows) + " c" +
                   std::to_string(m->channels()) + " " + depth_name(m->depth());
        } catch (...) {
            return std::string();
        }
    }

    // displayable — 8-bit view of _m; alpha is dropped for the BMP encoder
    cv::Mat displayable(const cv::Mat& _m) {
        cv::Mat work;
        switch (_m.depth()) {
        case CV_8U: work = _m; break;
        case CV_8S: _m.convertTo(work, CV_8U, 1.0, 128.0); break;
        case CV_16U: _m.convertTo(work, CV_8U, 1.0 / 256.0); break;
        case CV_32F:
        case CV_64F: {
            cv::Mat f;
            _m.convertTo(f, CV_32F);
            cv::patchNaNs(f, 0.0);
            cv::normalize(f, work, 0, 255, cv::NORM_MINMAX, CV_8U);
            break;
        }
        default: cv::normalize(_m, work, 0, 255, cv::NORM_MINMAX, CV_8U); break;
        }
        if (work.channels() == 4) cv::cvtColor(work, work, cv::COLOR_BGRA2BGR);
        return work;
    }

    alx::variant view_image(const alx::anyptr& _h) {
        try {
            cv::Mat* m = alx::anyptr_ex<cv::Mat>::as(_h);
            if (!m || m->empty()) return alx::variant();
            const int channels = m->channels();
            if (channels != 1 && channels != 3 && channels != 4) return alx::variant();
            cv::Mat show = displayable(*m);
            std::vector<unsigned char> buf;
            if (!cv::imencode(".bmp", show, buf)) return alx::variant();
            alx::varmap out;
            out["kind"] = alx::variant(std::string("image"));
            out["format"] = alx::variant(std::string("bmp"));
            out["data"] = alx::variant(alx::bytes(buf.data(), buf.size()));
            out["width"] = alx::variant(alx::int_64(show.cols));
            out["height"] = alx::variant(alx::int_64(show.rows));
            out["channels"] = alx::variant(alx::int_64(show.channels()));
            out["depth"] = alx::variant(std::string(depth_name(m->depth())));
            out["srcWidth"] = alx::variant(alx::int_64(m->cols));
            out["srcHeight"] = alx::variant(alx::int_64(m->rows));
            out["srcChannels"] = alx::variant(alx::int_64(channels));
            out["scaled"] = alx::variant(false);
            out["normalized"] = alx::variant(m->depth() != CV_8U);
            return alx::variant(std::move(out));
        } catch (...) {
            return alx::variant();
        }
    }

} // namespace

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#endif
    void alexis_script_load(alx::script::fwrap& _fw) {
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
    _fw.bind(alx::anyptr_ex<cv::Mat>::rid, "cv::Mat", view_desc, view_image);
    _fw.bind("imread", fn_imread);
    _fw.bind("imwrite", fn_imwrite);
    _fw.bind("imdecode", fn_imdecode);
    _fw.bind("imencode", fn_imencode);
    _fw.bind("create", fn_create);
    _fw.bind("clone", fn_clone);
    _fw.bind("info", fn_info);
    _fw.bind("version", fn_version);

    _fw.bind("resize", fn_resize);
    _fw.bind("crop", fn_crop);
    _fw.bind("rotate", fn_rotate);
    _fw.bind("flip", fn_flip);
    _fw.bind("transpose", fn_transpose);

    _fw.bind("cvt_color", fn_cvt_color);
    _fw.bind("threshold", fn_threshold);
    _fw.bind("split", fn_split);
    _fw.bind("merge", fn_merge);
    _fw.bind("convert", fn_convert);

    _fw.bind("blur", fn_blur);
    _fw.bind("gaussian_blur", fn_gaussian_blur);
    _fw.bind("median_blur", fn_median_blur);
    _fw.bind("bilateral", fn_bilateral);
    _fw.bind("sobel", fn_sobel);
    _fw.bind("laplacian", fn_laplacian);
    _fw.bind("canny", fn_canny);

    _fw.bind("erode", fn_erode);
    _fw.bind("dilate", fn_dilate);
    _fw.bind("morphology", fn_morphology);

    _fw.bind("line", fn_line);
    _fw.bind("rect", fn_rect);
    _fw.bind("circle", fn_circle);
    _fw.bind("ellipse", fn_ellipse);
    _fw.bind("text", fn_text);

    _fw.bind("add", fn_add);
    _fw.bind("subtract", fn_subtract);
    _fw.bind("absdiff", fn_absdiff);
    _fw.bind("bitwise_and", fn_bitwise_and);
    _fw.bind("bitwise_or", fn_bitwise_or);
    _fw.bind("bitwise_xor", fn_bitwise_xor);
    _fw.bind("bitwise_not", fn_bitwise_not);
    _fw.bind("blend", fn_blend);
}
