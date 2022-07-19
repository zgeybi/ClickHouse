#pragma once

#include <Parsers/IAST.h>
#include <DataTypes/IDataType.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Storages/IStorage_fwd.h>

namespace DB
{

class QueryPlan;

class Set;
using SetPtr = std::shared_ptr<Set>;
class InterpreterSelectWithUnionQuery;

/// Information on how to build set for the [GLOBAL] IN section.
class SubqueryForSet
{
public:
    SubqueryForSet();
    ~SubqueryForSet();

    SubqueryForSet(SubqueryForSet &&) noexcept;
    SubqueryForSet & operator=(SubqueryForSet &&) noexcept;

    void setSource(InterpreterSelectWithUnionQuery & interpreter, StoragePtr table_ = nullptr);

    bool hasSource() const;

    /// Returns query plan for the source of the set
    /// It would be removed from SubqueryForSet
    std::unique_ptr<QueryPlan> moveSource();

    /// Build this set from the result of the subquery.
    SetPtr set;

    /// If set, put the result into the table.
    /// This is a temporary table for transferring to remote servers for distributed query processing.
    StoragePtr table;

private:
    /// The source is obtained using the InterpreterSelectQuery subquery.
    std::unique_ptr<QueryPlan> source;
};

struct PreparedSetKey
{
    /// Prepared sets for tuple literals are indexed by the hash of the tree contents and by the desired
    /// data types of set elements (two different Sets can be required for two tuples with the same contents
    /// if left hand sides of the IN operators have different types).
    static PreparedSetKey forLiteral(const IAST & ast, DataTypes types_);

    /// Prepared sets for subqueries are indexed only by the AST contents because the type of the resulting
    /// set is fully determined by the subquery.
    static PreparedSetKey forSubquery(const IAST & ast);

    IAST::Hash ast_hash;
    DataTypes types; /// Empty for subqueries.

    bool operator==(const PreparedSetKey & other) const;

    struct Hash
    {
        UInt64 operator()(const PreparedSetKey & key) const { return key.ast_hash.first; }
    };
};

class PreparedSets
{
public:
    using SubqueriesForSets = std::unordered_map<String, SubqueryForSet>;

    SubqueryForSet & createOrGetSubquery(const String & subquery_id, const PreparedSetKey & key, SetPtr set);
    SubqueryForSet & getSubquery(const String & subquery_id);

    void setSet(const PreparedSetKey & key, SetPtr set_);
    SetPtr & getSet(const PreparedSetKey & key);

    /// Get subqueries and clear them
    SubqueriesForSets moveSubqueries();

    /// Returns all sets that match the given ast hash not checking types
    /// Used in KeyCondition and MergeTreeIndexConditionBloomFilter to make non exact match for types in PreparedSetKey
    std::vector<SetPtr> getByTreeHash(IAST::Hash ast_hash);

    bool empty() const;

private:
    std::unordered_map<PreparedSetKey, SetPtr, PreparedSetKey::Hash> sets;

    /// This is the information required for building sets
    SubqueriesForSets subqueries;
};

using PreparedSetsPtr = std::shared_ptr<PreparedSets>;

}
