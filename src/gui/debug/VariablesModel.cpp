#include "VariablesModel.h"

#include <QVariantMap>

VariablesModel::VariablesModel(QObject* _parent)
    : QAbstractItemModel(_parent) {}

VariablesModel::~VariablesModel() = default;

void VariablesModel::setRoot(const QString& _ref, bool _request) {
    beginResetModel();
    m_root = std::make_unique<node>();
    m_root->ref = _ref;
    m_root->has_children = true;
    endResetModel();
    if (_request && !_ref.isEmpty()) emit snapshotRequested(_ref, 0);
}

void VariablesModel::clear() {
    beginResetModel();
    m_root.reset();
    m_watchRef.clear();
    endResetModel();
}

void VariablesModel::apply(const QString& _ref, quint64 _ofst, const QVariantList& _nodes, bool _more) {
    if (!m_root) return;
    node* target = find(_ref);
    if (!target || _ofst != target->fetched) return; // duplicate or stale page
    target->pending = false;
    target->loaded = true;

    if (_nodes.isEmpty()) {
        target->more = false;
        return;
    }
    const QModelIndex parentIndex =
        target == m_root.get() ? QModelIndex() : createIndex(rowOf(target), 0, target);
    const int first = static_cast<int>(target->kids.size());
    beginInsertRows(parentIndex, first, first + static_cast<int>(_nodes.size()) - 1);
    for (const auto& nv : _nodes) {
        const QVariantMap m = nv.toMap();
        auto child = std::make_unique<node>();
        child->name = m.value(QStringLiteral("name")).toString();
        child->type = m.value(QStringLiteral("type")).toString();
        child->value = m.value(QStringLiteral("value")).toString();
        child->kind = m.value(QStringLiteral("kind")).toString();
        child->ref = m.value(QStringLiteral("ref")).toString();
        child->watching = !child->ref.isEmpty() && child->ref == m_watchRef;
        child->has_children = m.value(QStringLiteral("hasChildren")).toBool();
        child->parent = target;
        target->kids.push_back(std::move(child));
    }
    target->fetched += static_cast<quint64>(_nodes.size());
    target->more = _more;
    endInsertRows();
}

QModelIndex VariablesModel::index(int _row, int _column, const QModelIndex& _parent) const {
    if (!hasIndex(_row, _column, _parent)) return {};
    node* parent = nodeOf(_parent);
    if (!parent || _row >= static_cast<int>(parent->kids.size())) return {};
    return createIndex(_row, _column, parent->kids[_row].get());
}

QModelIndex VariablesModel::parent(const QModelIndex& _child) const {
    if (!_child.isValid()) return {};
    node* n = nodeOf(_child);
    if (!n || !n->parent || n->parent == m_root.get()) return {};
    return createIndex(rowOf(n->parent), 0, n->parent);
}

int VariablesModel::rowCount(const QModelIndex& _parent) const {
    const node* n = nodeOf(_parent);
    return n ? static_cast<int>(n->kids.size()) : 0;
}

int VariablesModel::columnCount(const QModelIndex& _parent) const {
    (void) _parent;
    return 3;
}

QVariant VariablesModel::data(const QModelIndex& _index, int _role) const {
    if (!_index.isValid()) return {};
    const node* n = nodeOf(_index);
    if (!n) return {};
    if (_role == Qt::DisplayRole) {
        if (_index.column() == 0) return n->name;
        if (_index.column() == 2) return {};
        if (!n->value.isEmpty()) return n->value;
        return n->has_children ? QStringLiteral("<%1>").arg(n->type) : n->value;
    }
    if (_role == Qt::CheckStateRole && _index.column() == 2 && n->kind == QLatin1String("handle") &&
        !n->ref.isEmpty())
        return n->watching ? Qt::Checked : Qt::Unchecked;
    if (_role == Qt::ToolTipRole && _index.column() == 1) {
        QString tip = n->value;
        if (!n->type.isEmpty()) tip += QStringLiteral("\n[%1]").arg(n->type);
        return tip;
    }
    if (_role == Qt::ToolTipRole && _index.column() == 2 && n->kind == QLatin1String("handle"))
        return QStringLiteral("打开视图");
    return {};
}

bool VariablesModel::setData(const QModelIndex& _index, const QVariant& _value, int _role) {
    if (_role != Qt::CheckStateRole || _index.column() != 2) return false;
    node* n = nodeOf(_index);
    if (!n || n->kind != QLatin1String("handle") || n->ref.isEmpty()) return false;
    const bool on = _value.toInt() == Qt::Checked;
    if (on) {
        clearWatching(n);
        m_watchRef = n->ref;
    } else {
        m_watchRef.clear();
    }
    if (n->watching != on) {
        n->watching = on;
        emit dataChanged(_index, _index, {Qt::CheckStateRole});
    }
    emit watchToggled(n->ref, n->type, on);
    return true;
}

Qt::ItemFlags VariablesModel::flags(const QModelIndex& _index) const {
    Qt::ItemFlags f = QAbstractItemModel::flags(_index);
    const node* n = nodeOf(_index);
    if (n && _index.column() == 2 && n->kind == QLatin1String("handle") && !n->ref.isEmpty())
        f |= Qt::ItemIsUserCheckable;
    return f;
}

QVariant VariablesModel::headerData(int _section, Qt::Orientation _orientation, int _role) const {
    if (_role != Qt::DisplayRole) return {};
    if (_orientation != Qt::Horizontal) return {};
    if (_section == 0) return QStringLiteral("名称");
    if (_section == 2) return QStringLiteral("视图");
    return QStringLiteral("值");
}

bool VariablesModel::hasChildren(const QModelIndex& _parent) const {
    const node* n = nodeOf(_parent);
    return n && (n->has_children || n->more);
}

bool VariablesModel::canFetchMore(const QModelIndex& _parent) const {
    const node* n = nodeOf(_parent);
    return n && n->has_children && !n->pending && (!n->loaded || n->more);
}

void VariablesModel::fetchMore(const QModelIndex& _parent) {
    node* n = nodeOf(_parent);
    if (!n || !n->has_children || n->pending || (n->loaded && !n->more)) return;
    n->pending = true;
    emit snapshotRequested(n->ref, n->fetched);
}

VariablesModel::node* VariablesModel::nodeOf(const QModelIndex& _index) const {
    if (!_index.isValid()) return m_root.get();
    return static_cast<node*>(_index.internalPointer());
}

VariablesModel::node* VariablesModel::find(const QString& _ref) const {
    if (!m_root) return nullptr;
    if (m_root->ref == _ref) return m_root.get();
    std::vector<node*> stack;
    for (auto& k : m_root->kids) stack.push_back(k.get());
    while (!stack.empty()) {
        node* n = stack.back();
        stack.pop_back();
        if (n->ref == _ref) return n;
        for (auto& k : n->kids) stack.push_back(k.get());
    }
    return nullptr;
}

int VariablesModel::rowOf(const node* _n) const {
    if (!_n || !_n->parent) return 0;
    const auto& kids = _n->parent->kids;
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i].get() == _n) return static_cast<int>(i);
    return 0;
}

void VariablesModel::clearWatching(node* _exception) {
    if (!m_root) return;
    std::vector<node*> stack;
    for (auto& k : m_root->kids) stack.push_back(k.get());
    while (!stack.empty()) {
        node* n = stack.back();
        stack.pop_back();
        if (n != _exception && n->watching) {
            n->watching = false;
            const QModelIndex idx = createIndex(rowOf(n), 2, n);
            emit dataChanged(idx, idx, {Qt::CheckStateRole});
        }
        for (auto& k : n->kids) stack.push_back(k.get());
    }
}
