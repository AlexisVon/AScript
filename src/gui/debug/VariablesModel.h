#ifndef AGUI_VARIABLESMODEL_H
#define AGUI_VARIABLESMODEL_H

#include <QAbstractItemModel>
#include <QVariantList>
#include <QString>

#include <memory>
#include <vector>

// VariablesModel — lazily expanded variable tree; children arrive from snapshot replies
class VariablesModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit VariablesModel(QObject* _parent = nullptr);
    ~VariablesModel() override;

    // setRoot — clear and request the first page of _ref (the paused frame)
    // _request false when the caller has already asked for the first page
    void setRoot(const QString& _ref, bool _request = true);
    void clear();
    // apply — append one page of _ref's children; unknown or stale pages are ignored
    void apply(const QString& _ref, quint64 _ofst, const QVariantList& _nodes, bool _more);

    QModelIndex index(int _row, int _column, const QModelIndex& _parent) const override;
    QModelIndex parent(const QModelIndex& _child) const override;
    int rowCount(const QModelIndex& _parent) const override;
    int columnCount(const QModelIndex& _parent) const override;
    QVariant data(const QModelIndex& _index, int _role) const override;
    bool setData(const QModelIndex& _index, const QVariant& _value, int _role) override;
    Qt::ItemFlags flags(const QModelIndex& _index) const override;
    QVariant headerData(int _section, Qt::Orientation _orientation, int _role) const override;
    bool hasChildren(const QModelIndex& _parent) const override;
    bool canFetchMore(const QModelIndex& _parent) const override;
    void fetchMore(const QModelIndex& _parent) override;

signals:
    void snapshotRequested(const QString& _ref, quint64 _ofst);
    void watchToggled(const QString& _ref, const QString& _type, bool _on);

private:
    struct node {
        QString name;
        QString type;
        QString value;
        QString kind;
        QString ref;
        bool watching = false; // this node's view switch is on
        bool has_children = false;
        bool loaded = false; // at least one page has been received
        bool more = false; // the runner reported another page
        bool pending = false; // a page request is in flight
        quint64 fetched = 0; // children received so far
        node* parent = nullptr;
        std::vector<std::unique_ptr<node>> kids;
    };

    node* nodeOf(const QModelIndex& _index) const;
    node* find(const QString& _ref) const;
    int rowOf(const node* _n) const;
    void clearWatching(node* _exception);

    std::unique_ptr<node> m_root;
    QString m_watchRef;
};

#endif // AGUI_VARIABLESMODEL_H
