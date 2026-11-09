#ifndef FILETREEDOCK_H
#define FILETREEDOCK_H

#include <QSortFilterProxyModel>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QTimer;
class QTreeView;

// NameFilterProxy — file tree filter: a row matches when its name contains the
// filter text, or (for directories) when any descendant matches (recursive)
class NameFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void refilter() { invalidateFilter(); }

    void setNeedle(const QString& _needle) {
        if (m_needle == _needle) return;
        m_needle = _needle;
        invalidateFilter();
    }

    // icons + per-type text colors live in the model: QStyledItemDelegate
    // re-reads DecorationRole/ForegroundRole over anything a delegate pre-sets
    QVariant data(const QModelIndex& _index, int _role) const override;
    bool dropMimeData(const QMimeData* _data, Qt::DropAction _action, int _row, int _column,
                      const QModelIndex& _parent) override;

signals:
    void fileMoved(const QString& _from, const QString& _to);

protected:
    bool filterAcceptsRow(int _row, const QModelIndex& _parent) const override;

private:
    QString m_needle;
};

// FileTreeDock — directory tree panel: search filter + QFileSystemModel + context
// menu + drag & drop. Signals report user actions; the main window owns the
// orchestration (open file, activate project, compile, etc.).
class FileTreeDock : public QWidget {
    Q_OBJECT

public:
    explicit FileTreeDock(QWidget* _parent = nullptr);

    void setRootPath(const QString& _path);
    void clearRoot(); // close the folder: empty state, tree hidden
    void refreshModel(); // fetchMore after file creation
    void selectFile(const QString& _path); // select & scroll to a file in the tree
    QString rootPath() const { return m_lastDir; }

signals:
    void fileOpened(const QString& _path); // text file double-click (.axc/.json/.txt/.md)
    void axSelected(const QString& _path); // .ax double-click → activate project (open main)
    void axpSelected(const QString& _path); // .axp double-click → readonly dock
    void propsRequested(const QString& _path); // right-click "属性" → property panel
    void runRequested(const QString& _path); // right-click "运行" (.ax/.axc/.axp)
    void runDebugRequested(const QString& _path); // right-click "调试运行"
    void axConfigRequested(const QString& _path); // right-click "配置(.ax 表单)…"
    void compileRequested(const QString& _path); // right-click "编译…" (.ax/.axc)
    void compileTreeRequested(); // empty area "编译文件树…"
    void logMessage(const QString& _msg); // system log

private:
    void buildTree();
    void applyFilter(const QString& _text);
    void expandAllDirs(const QModelIndex& _root);
    void fileTreeMenu(const QPoint& _pos);

    QFileSystemModel* m_fsModel;
    NameFilterProxy* m_proxy;
    QTreeView* m_tree;
    QLineEdit* m_searchBox;
    QLabel* m_empty;
    QTimer* m_filterTimer;
    QString m_lastDir;
};

#endif // FILETREEDOCK_H
