#include "PropsPanel.h"

#include "EmbedWidget.h"
#include "core/project.h"

#include "ajson.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

    // human_size — 1024-based size string
    QString human_size(qint64 _bytes) {
        if (_bytes < 1024) return QString::number(_bytes) + " B";
        if (_bytes < 1024 * 1024) return QString::number(_bytes / 1024.0, 'f', 1) + " KB";
        return QString::number(_bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    }

    // addRow — key-value row into a layout (key dim, value selectable)
    void addRow(QVBoxLayout* _lay, const QString& _key, const QString& _value, QWidget* _panel) {
        auto* row = new QHBoxLayout();
        auto* key = new QLabel(_key + ":", _panel);
        key->setStyleSheet("color: #928374;");
        auto* value = new QLabel(_value, _panel);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        row->addWidget(key);
        row->addWidget(value, 1);
        _lay->addLayout(row);
    }

} // namespace

PropsPanel::PropsPanel(QWidget* _parent)
    : QWidget(_parent) {}

void PropsPanel::show(const QString& _path, QWidget* _parent) {
    const QFileInfo fi(_path);
    auto* panel = new EmbedWidget(_parent);
    panel->setFixedWidth(360);
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(10, 8, 10, 8);

    // title bar
    lay->addLayout(gui::titleRow(fi.fileName(), panel, [panel] { panel->hide(); }));

    auto* tabs = new QTabWidget(panel);

    // ── page 1: general ──
    auto* general = new QWidget(tabs);
    auto* genLay = new QVBoxLayout(general);
    genLay->setContentsMargins(6, 6, 6, 6);
    addRow(genLay, "路径", fi.absoluteFilePath(), panel);
    addRow(genLay, "类型",
           fi.isDir() ? "文件夹" : fi.suffix().isEmpty() ? "文件"
                                                         : fi.suffix().toUpper() + " 文件",
           panel);
    if (!fi.isDir()) addRow(genLay, "大小", human_size(fi.size()), panel);
    addRow(genLay, "修改时间", fi.lastModified().toString("yyyy-MM-dd HH:mm:ss"), panel);
    addRow(genLay, "权限",
           QString(fi.isReadable() ? "可读 " : "只读 ") + (fi.isWritable() ? "可写" : "只写"), panel);
    genLay->addStretch();
    tabs->addTab(general, "常规");

    // ── page 2: .axp metadata ──
    if (fi.suffix() == "axp") {
        auto* axpPage = new QWidget(tabs);
        auto* axpLay = new QVBoxLayout(axpPage);
        axpLay->setContentsMargins(6, 6, 6, 6);

        core::project::axp_view v;
        std::string err;
        if (core::project::unpack_axp(_path.toStdString(), v, err)) {
            // ordered field descriptor: key, label, isTime (Unix seconds → readable)
            struct Field {
                const char* key;
                const char* label;
                bool isTime;
            };
            static const Field kOuterFields[] = {
                {"etype", "引擎版本", false},
                {"vtype", "扩展版本", false},
                {"time", "编译时间", true},
                {"file", "源文件", false},
            };
            static const Field kInnerFields[] = {
                {"compressed", "压缩", false},
                {"size", "原始大小", false},
                {"hintsize", "hint 大小", false},
            };
            auto displayValue = [](const alx::json_value& _val, bool _isTime) -> QString {
                if (_isTime && _val.is<alx::int_64>())
                    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(_val.to<alx::int_64>()))
                        .toString("yyyy-MM-dd HH:mm:ss");
                if (_val.is<bool>()) return _val.to<bool>() ? "是" : "否";
                if (_val.is<alx::int_64>()) return QString::number(_val.to<alx::int_64>());
                if (_val.is<double>()) return QString::number(_val.to<double>());
                if (_val.is<std::string>()) return QString::fromStdString(_val.to<std::string>());
                return QString::fromStdString(alx::json_doc::to_string(_val, true));
            };
            if (!v.info_text.empty()) {
                bool ok = false;
                alx::json_value jv = alx::json_doc::from_value(v.info_text, &ok);
                if (ok && jv.is<alx::json_object>()) {
                    const auto& obj = jv.to<alx::json_object>();
                    for (const auto& f : kOuterFields)
                        if (obj.contain(f.key))
                            addRow(axpLay, f.label, displayValue(obj.value(f.key), f.isTime), panel);
                    if (obj.contain("info") && obj.value("info").is<alx::json_object>()) {
                        const auto& inner = obj.value("info").to<alx::json_object>();
                        for (const auto& f : kInnerFields)
                            if (inner.contain(f.key))
                                addRow(axpLay, f.label, displayValue(inner.value(f.key), f.isTime), panel);
                    }
                }
            }
            addRow(axpLay, "模块数", QString::number(v.modules.size()), panel);
        } else {
            auto* errLabel = new QLabel("解包失败: " + QString::fromStdString(err), axpPage);
            errLabel->setStyleSheet("color: #fb4934;");
            errLabel->setWordWrap(true);
            axpLay->addWidget(errLabel);
        }
        axpLay->addStretch();
        tabs->addTab(axpPage, ".axp 元数据");
    }

    if (fi.suffix() == "axp") {
        lay->addWidget(tabs);
    } else { // single page: no lonely tab bar
        tabs->removeTab(0);
        general->setParent(panel);
        lay->addWidget(general);
        general->show(); // removeTab hid the page; adjustSize ignores hidden children
        delete tabs;
    }

    QObject::connect(panel, &EmbedWidget::sig_visible, panel,
                     [panel](bool _v) { if (!_v) panel->deleteLater(); });
    panel->adjustSize();
    panel->move_center();
    panel->show();
}
