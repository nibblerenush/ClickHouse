#include <Interpreters/MySQL/InterpretersMySQLDDLQuery.h>

#include <Parsers/IAST.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTDataType.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/ASTIndexDeclaration.h>
#include <Parsers/MySQL/ASTCreateQuery.h>
#include <Parsers/MySQL/ASTAlterCommand.h>
#include <Parsers/MySQL/ASTDeclareColumn.h>
#include <Parsers/MySQL/ASTDeclareOption.h>
#include <Parsers/MySQL/ASTCreateDefines.h>

#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeNullable.h>
#include <Parsers/MySQL/ASTDeclareIndex.h>
#include <Common/quoteString.h>
#include <Common/assert_cast.h>
#include <Interpreters/getTableOverride.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/InterpreterCreateQuery.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/applyTableOverride.h>
#include <Storages/IStorage.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_TYPE;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int EMPTY_LIST_OF_COLUMNS_PASSED;
}

namespace MySQLInterpreter
{

static inline String resolveDatabase(
    const String & database_in_query, const String & replica_mysql_database, const String & replica_clickhouse_database, ContextPtr context)
{
    if (!database_in_query.empty())
    {
        if (database_in_query == replica_mysql_database)
        {
            /// USE other_database_name; CREATE TABLE replica_mysql_database.table_name;
            /// USE replica_mysql_database; CREATE TABLE replica_mysql_database.table_name;
            return replica_clickhouse_database;
        }

        /// USE other_database_name; CREATE TABLE other_database_name.table_name;
        /// USE replica_mysql_database; CREATE TABLE other_database_name.table_name;
        return "";
    }

    /// When USE other_database_name; CREATE TABLE table_name;
    /// context.getCurrentDatabase() is always return `default database`
    /// When USE replica_mysql_database; CREATE TABLE table_name;
    /// context.getCurrentDatabase() is always return replica_clickhouse_database
    const String & current_database = context->getCurrentDatabase();
    return current_database != replica_clickhouse_database ? "" : replica_clickhouse_database;
}

NamesAndTypesList getColumnsList(const ASTExpressionList * columns_definition)
{
    NamesAndTypesList columns_name_and_type;
    for (const auto & declare_column_ast : columns_definition->children)
    {
        const auto & declare_column = declare_column_ast->as<MySQLParser::ASTDeclareColumn>();

        if (!declare_column || !declare_column->data_type)
            throw Exception(ErrorCodes::UNKNOWN_TYPE, "Missing type in definition of column.");

        bool is_nullable = true;
        bool is_unsigned = false;
        if (declare_column->column_options)
        {
            if (const auto * options = declare_column->column_options->as<MySQLParser::ASTDeclareOptions>())
            {
                if (options->changes.contains("is_null"))
                    is_nullable = options->changes.at("is_null")->as<ASTLiteral>()->value.safeGet<UInt64>();

                if (options->changes.contains("is_unsigned"))
                    is_unsigned = options->changes.at("is_unsigned")->as<ASTLiteral>()->value.safeGet<UInt64>();
            }
        }

        ASTPtr data_type = declare_column->data_type;
        auto * data_type_node = data_type->as<ASTDataType>();

        if (data_type_node)
        {
            String type_name_upper = Poco::toUpper(data_type_node->name);

            if (is_unsigned)
            {
                /// For example(in MySQL): CREATE TABLE test(column_name INT NOT NULL ... UNSIGNED)
                if (type_name_upper.contains("INT") && !endsWith(type_name_upper, "SIGNED")
                    && !endsWith(type_name_upper, "UNSIGNED"))
                    data_type_node->name = type_name_upper + " UNSIGNED";
            }

            if (type_name_upper == "SET")
                data_type_node->arguments.reset();

            /// Transforms MySQL ENUM's list of strings to ClickHouse string-integer pairs
            /// For example ENUM('a', 'b', 'c') -> ENUM('a'=1, 'b'=2, 'c'=3)
            /// Elements on a position further than 32767 are assigned negative values, starting with -32768.
            /// Note: Enum would be transformed to Enum8 if number of elements is less then 128, otherwise it would be transformed to Enum16.
            if (type_name_upper.contains("ENUM"))
            {
                UInt16 i = 0;
                for (ASTPtr & child : data_type_node->arguments->children)
                {
                    auto new_child = std::make_shared<ASTFunction>();
                    new_child->name = "equals";
                    auto * literal = child->as<ASTLiteral>();

                    new_child->arguments = std::make_shared<ASTExpressionList>();
                    new_child->arguments->children.push_back(std::make_shared<ASTLiteral>(literal->value.safeGet<String>()));
                    new_child->arguments->children.push_back(std::make_shared<ASTLiteral>(Int16(++i)));
                    child = new_child;
                }
            }

            if (type_name_upper == "DATE")
                data_type_node->name = "Date32";
        }
        if (is_nullable)
            data_type = makeASTDataType("Nullable", data_type);

        columns_name_and_type.emplace_back(declare_column->name, DataTypeFactory::instance().get(data_type));
    }

    return columns_name_and_type;
}

static ColumnsDescription createColumnsDescription(const NamesAndTypesList & columns_name_and_type, const ASTExpressionList * columns_definition)
{
    if (columns_name_and_type.size() != columns_definition->children.size())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Columns of different size provided.");

    ColumnsDescription columns_description;

    /// FIXME: we could write it like auto [a, b] = std::tuple(x, y),
    /// but this produce endless recursion in gcc-11, and leads to SIGSEGV
    /// (see git blame for details).
    auto column_name_and_type = columns_name_and_type.begin();
    const auto * declare_column_ast = columns_definition->children.begin();
    for (; column_name_and_type != columns_name_and_type.end(); ++column_name_and_type, ++declare_column_ast)
    {
        const auto & declare_column = (*declare_column_ast)->as<MySQLParser::ASTDeclareColumn>();
        String comment;
        if (declare_column->column_options)
            if (const auto * options = declare_column->column_options->as<MySQLParser::ASTDeclareOptions>())
                if (options->changes.contains("comment"))
                    comment = options->changes.at("comment")->as<ASTLiteral>()->value.safeGet<String>();

        ColumnDescription column_description(column_name_and_type->name, column_name_and_type->type);
        if (!comment.empty())
            column_description.comment = std::move(comment);

        columns_description.add(column_description);
    }

    return columns_description;
}

static NamesAndTypesList getNames(const ASTFunction & expr, ContextPtr context, const NamesAndTypesList & columns)
{
    if (expr.arguments->children.empty())
        return NamesAndTypesList{};

    ASTPtr temp_ast = expr.clone();
    auto syntax = TreeRewriter(context).analyze(temp_ast, columns);
    auto required_columns = ExpressionAnalyzer(temp_ast, syntax, context).getActionsDAG(false).getRequiredColumns();
    return required_columns;
}

static NamesAndTypesList modifyPrimaryKeysToNonNullable(const NamesAndTypesList & primary_keys, NamesAndTypesList & columns)
{
    /// https://dev.mysql.com/doc/refman/5.7/en/create-table.html#create-table-indexes-keys
    /// PRIMARY KEY:
    /// A unique index where all key columns must be defined as NOT NULL.
    /// If they are not explicitly declared as NOT NULL, MySQL declares them so implicitly (and silently).
    /// A table can have only one PRIMARY KEY. The name of a PRIMARY KEY is always PRIMARY,
    /// which thus cannot be used as the name for any other kind of index.
    NamesAndTypesList non_nullable_primary_keys;
    for (const auto & primary_key : primary_keys)
    {
        if (!primary_key.type->isNullable())
            non_nullable_primary_keys.emplace_back(primary_key);
        else
        {
            non_nullable_primary_keys.emplace_back(
                NameAndTypePair(primary_key.name, assert_cast<const DataTypeNullable *>(primary_key.type.get())->getNestedType()));

            for (auto & column : columns)
            {
                if (column.name == primary_key.name)
                    column.type = assert_cast<const DataTypeNullable *>(column.type.get())->getNestedType();
            }
        }
    }

    return non_nullable_primary_keys;
}

static std::tuple<NamesAndTypesList, NamesAndTypesList, NamesAndTypesList, NameSet> getKeys(
    ASTExpressionList * columns_definition, ASTExpressionList * indices_define, ContextPtr context, NamesAndTypesList & columns)
{
    NameSet increment_columns;
    auto keys = makeASTFunction("tuple");
    auto unique_keys = makeASTFunction("tuple");
    auto primary_keys = makeASTFunction("tuple");

    if (indices_define && !indices_define->children.empty())
    {
        NameSet columns_name_set;
        const Names & columns_name = columns.getNames();
        columns_name_set.insert(columns_name.begin(), columns_name.end());

        const auto & remove_prefix_key = [&](const ASTPtr & node) -> ASTPtr
        {
            auto res = std::make_shared<ASTExpressionList>();
            for (const auto & index_expression : node->children)
            {
                res->children.emplace_back(index_expression);

                if (const auto & function = index_expression->as<ASTFunction>())
                {
                    /// column_name(int64 literal)
                    if (columns_name_set.contains(function->name) && function->arguments->children.size() == 1)
                    {
                        const auto & prefix_limit = function->arguments->children[0]->as<ASTLiteral>();

                        if (prefix_limit && isInt64OrUInt64FieldType(prefix_limit->value.getType()))
                            res->children.back() = std::make_shared<ASTIdentifier>(function->name);
                    }
                }
            }
            return res;
        };

        for (const auto & declare_index_ast : indices_define->children)
        {
            const auto & declare_index = declare_index_ast->as<MySQLParser::ASTDeclareIndex>();
            const auto & index_columns = remove_prefix_key(declare_index->index_columns);

            /// flatten
            if (startsWith(declare_index->index_type, "KEY_"))
                keys->arguments->children.insert(keys->arguments->children.end(),
                    index_columns->children.begin(), index_columns->children.end());
            else if (startsWith(declare_index->index_type, "UNIQUE_"))
                unique_keys->arguments->children.insert(unique_keys->arguments->children.end(),
                    index_columns->children.begin(), index_columns->children.end());
            if (startsWith(declare_index->index_type, "PRIMARY_KEY_"))
                primary_keys->arguments->children.insert(primary_keys->arguments->children.end(),
                    index_columns->children.begin(), index_columns->children.end());
        }
    }

    for (const auto & declare_column_ast : columns_definition->children)
    {
        const auto & declare_column = declare_column_ast->as<MySQLParser::ASTDeclareColumn>();

        if (declare_column->column_options)
        {
            if (const auto * options = declare_column->column_options->as<MySQLParser::ASTDeclareOptions>())
            {
                if (options->changes.contains("unique_key"))
                    unique_keys->arguments->children.emplace_back(std::make_shared<ASTIdentifier>(declare_column->name));

                if (options->changes.contains("primary_key"))
                    primary_keys->arguments->children.emplace_back(std::make_shared<ASTIdentifier>(declare_column->name));

                if (options->changes.contains("auto_increment"))
                    increment_columns.emplace(declare_column->name);
            }
        }
    }

    const auto & primary_keys_names_and_types = getNames(*primary_keys, context, columns);
    const auto & non_nullable_primary_keys_names_and_types = modifyPrimaryKeysToNonNullable(primary_keys_names_and_types, columns);
    return std::make_tuple(non_nullable_primary_keys_names_and_types, getNames(*unique_keys, context, columns), getNames(*keys, context, columns), increment_columns);
}

static String getUniqueColumnName(NamesAndTypesList columns_name_and_type, const String & prefix)
{
    const auto & is_unique = [&](const String & column_name)
    {
        for (const auto & column_name_and_type : columns_name_and_type)
        {
            if (column_name_and_type.name == column_name)
                return false;
        }

        return true;
    };

    if (is_unique(prefix))
        return prefix;

    for (size_t index = 0; ; ++index)
    {
        const String & cur_name = prefix + "_" + toString(index);
        if (is_unique(cur_name))
            return cur_name;
    }
}

static ASTPtr getPartitionPolicy(const NamesAndTypesList & primary_keys)
{
    const auto & numbers_partition = [&](const String & column_name, size_t type_max_size) -> ASTPtr
    {
        if (type_max_size <= 1000)
            return std::make_shared<ASTIdentifier>(column_name);

        return makeASTFunction("intDiv", std::make_shared<ASTIdentifier>(column_name),
           std::make_shared<ASTLiteral>(static_cast<UInt64>(type_max_size / 1000)));
    };

    ASTPtr best_partition;
    size_t best_size = 0;
    for (const auto & primary_key : primary_keys)
    {
        DataTypePtr type = primary_key.type;
        WhichDataType which(type);

        if (which.isNullable())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "MySQL's primary key must be not null, it is a bug.");

        if (which.isDate() || which.isDate32() || which.isDateTime() || which.isDateTime64())
        {
            /// In any case, date or datetime is always the best partitioning key
            return makeASTFunction("toYYYYMM", std::make_shared<ASTIdentifier>(primary_key.name));
        }

        if (type->haveMaximumSizeOfValue() && (!best_size || type->getSizeOfValueInMemory() < best_size))
        {
            if (which.isInt8() || which.isUInt8())
            {
                best_size = type->getSizeOfValueInMemory();
                best_partition = numbers_partition(primary_key.name, std::numeric_limits<UInt8>::max());
            }
            else if (which.isInt16() || which.isUInt16())
            {
                best_size = type->getSizeOfValueInMemory();
                best_partition = numbers_partition(primary_key.name, std::numeric_limits<UInt16>::max());
            }
            else if (which.isInt32() || which.isUInt32())
            {
                best_size = type->getSizeOfValueInMemory();
                best_partition = numbers_partition(primary_key.name, std::numeric_limits<UInt32>::max());
            }
            else if (which.isInt64() || which.isUInt64())
            {
                best_size = type->getSizeOfValueInMemory();
                best_partition = numbers_partition(primary_key.name, std::numeric_limits<UInt64>::max());
            }
        }
    }

    return best_partition;
}

static ASTPtr getOrderByPolicy(
    const NamesAndTypesList & primary_keys, const NamesAndTypesList & unique_keys, const NamesAndTypesList & keys, const NameSet & increment_columns)
{
    NameSet order_by_columns_set;
    std::deque<NamesAndTypesList> order_by_columns_list;

    const auto & add_order_by_expression = [&](const NamesAndTypesList & names_and_types)
    {
        NamesAndTypesList increment_keys;
        NamesAndTypesList non_increment_keys;

        for (const auto & [name, type] : names_and_types)
        {
            if (order_by_columns_set.contains(name))
                continue;

            if (increment_columns.contains(name))
            {
                order_by_columns_set.emplace(name);
                increment_keys.emplace_back(NameAndTypePair(name, type));
            }
            else
            {
                order_by_columns_set.emplace(name);
                non_increment_keys.emplace_back(NameAndTypePair(name, type));
            }
        }

        order_by_columns_list.emplace_back(increment_keys);
        order_by_columns_list.emplace_front(non_increment_keys);
    };

    /// primary_key[not increment], key[not increment], unique[not increment], unique[increment], key[increment], primary_key[increment]
    add_order_by_expression(unique_keys);
    add_order_by_expression(keys);
    add_order_by_expression(primary_keys);

    auto order_by_expression = std::make_shared<ASTFunction>();
    order_by_expression->name = "tuple";
    order_by_expression->arguments = std::make_shared<ASTExpressionList>();

    for (const auto & order_by_columns : order_by_columns_list)
    {
        for (const auto & [name, type] : order_by_columns)
        {
            order_by_expression->arguments->children.emplace_back(std::make_shared<ASTIdentifier>(name));

            if (type->isNullable())
                order_by_expression->arguments->children.back() = makeASTFunction("assumeNotNull", order_by_expression->arguments->children.back());
        }
    }

    return order_by_expression;
}

void InterpreterCreateImpl::validate(const InterpreterCreateImpl::TQuery & create_query, ContextPtr)
{
    if (!create_query.like_table)
    {
        bool missing_columns_definition = true;
        if (create_query.columns_list)
        {
            const auto & create_defines = create_query.columns_list->as<MySQLParser::ASTCreateDefines>();
            if (create_defines && create_defines->columns && !create_defines->columns->children.empty())
                missing_columns_definition = false;
        }
        if (missing_columns_definition)
            throw Exception(ErrorCodes::EMPTY_LIST_OF_COLUMNS_PASSED, "Missing definition of columns.");
    }
}

ASTs InterpreterCreateImpl::getRewrittenQueries(
    const TQuery & create_query, ContextPtr context, const String & mapped_to_database, const String & mysql_database)
{
    if (resolveDatabase(create_query.database, mysql_database, mapped_to_database, context) != mapped_to_database)
        return {};

    if (create_query.like_table)
    {
        auto * table_like = create_query.like_table->as<ASTTableIdentifier>();
        if (table_like->compound() && table_like->getTableId().database_name != mysql_database)
            return {};
        String table_name = table_like->shortName();
        ASTPtr rewritten_create_ast = DatabaseCatalog::instance().getDatabase(mapped_to_database)->getCreateTableQuery(table_name, context);
        auto * create_ptr = rewritten_create_ast->as<ASTCreateQuery>();
        create_ptr->setDatabase(mapped_to_database);
        create_ptr->setTable(create_query.table);
        create_ptr->uuid = UUIDHelpers::generateV4();
        create_ptr->if_not_exists = create_query.if_not_exists;
        return ASTs{rewritten_create_ast};
    }

    auto rewritten_query = std::make_shared<ASTCreateQuery>();
    const auto & create_defines = create_query.columns_list->as<MySQLParser::ASTCreateDefines>();

    NamesAndTypesList columns_name_and_type = getColumnsList(create_defines->columns);
    const auto & [primary_keys, unique_keys, keys, increment_columns] = getKeys(create_defines->columns, create_defines->indices, context, columns_name_and_type);
    ColumnsDescription columns_description = createColumnsDescription(columns_name_and_type, create_defines->columns);

    if (primary_keys.empty())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "The {}.{} cannot be materialized, because there is no primary keys.",
            backQuoteIfNeed(mysql_database), backQuoteIfNeed(create_query.table));

    auto columns = std::make_shared<ASTColumns>();

    const auto & create_materialized_column_declaration = [&](const String & name, const String & type, const auto & default_value)
    {
        auto column_declaration = std::make_shared<ASTColumnDeclaration>();
        column_declaration->name = name;
        column_declaration->type = makeASTDataType(type);
        column_declaration->default_specifier = "MATERIALIZED";
        column_declaration->default_expression = std::make_shared<ASTLiteral>(default_value);
        column_declaration->children.emplace_back(column_declaration->type);
        column_declaration->children.emplace_back(column_declaration->default_expression);
        return column_declaration;
    };

    /// Add _sign and _version columns.
    String sign_column_name = getUniqueColumnName(columns_name_and_type, "_sign");
    String version_column_name = getUniqueColumnName(columns_name_and_type, "_version");
    columns->set(columns->columns, InterpreterCreateQuery::formatColumns(columns_description));
    columns->columns->children.emplace_back(create_materialized_column_declaration(sign_column_name, "Int8", static_cast<UInt64>(1)));
    columns->columns->children.emplace_back(create_materialized_column_declaration(version_column_name, "UInt64", UInt64(1)));

    /// Add minmax skipping index for _version column.
    auto index_expr = std::make_shared<ASTIdentifier>(version_column_name);
    auto index_type = makeASTFunction("minmax");
    index_type->no_empty_args = true;
    auto version_index = std::make_shared<ASTIndexDeclaration>(index_expr, index_type, version_column_name);
    version_index->granularity = 1;

    ASTPtr indices = std::make_shared<ASTExpressionList>();
    indices->children.push_back(version_index);
    columns->set(columns->indices, indices);

    auto storage = std::make_shared<ASTStorage>();

    /// The `partition by` expression must use primary keys, otherwise the primary keys will not be merge.
    if (ASTPtr partition_expression = getPartitionPolicy(primary_keys))
        storage->set(storage->partition_by, partition_expression);

    /// The `order by` expression must use primary keys, otherwise the primary keys will not be merge.
    if (ASTPtr order_by_expression = getOrderByPolicy(primary_keys, unique_keys, keys, increment_columns))
        storage->set(storage->order_by, order_by_expression);

    storage->set(storage->engine, makeASTFunction("ReplacingMergeTree", std::make_shared<ASTIdentifier>(version_column_name)));

    rewritten_query->setDatabase(mapped_to_database);
    rewritten_query->setTable(create_query.table);
    rewritten_query->if_not_exists = create_query.if_not_exists;
    rewritten_query->set(rewritten_query->storage, storage);
    rewritten_query->set(rewritten_query->columns_list, columns);

    if (auto override_ast = tryGetTableOverride(mapped_to_database, create_query.table))
    {
        const auto & override = override_ast->as<const ASTTableOverride &>();
        applyTableOverrideToCreateQuery(override, rewritten_query.get());
    }

    return ASTs{rewritten_query};
}

void InterpreterDropImpl::validate(const InterpreterDropImpl::TQuery & /*query*/, ContextPtr /*context*/)
{
}

ASTs InterpreterDropImpl::getRewrittenQueries(
    const InterpreterDropImpl::TQuery & drop_query, ContextPtr context, const String & mapped_to_database, const String & mysql_database)
{
    /// Skip drop database|view|dictionary|others
    if (drop_query.kind != TQuery::Kind::Table)
        return {};
    TQuery::QualifiedNames tables = drop_query.names;
    ASTs rewritten_querys;
    for (const auto & table: tables)
    {
        const auto & database_name = resolveDatabase(table.schema, mysql_database, mapped_to_database, context);
        if (database_name != mapped_to_database)
            continue;
        auto rewritten_query = std::make_shared<ASTDropQuery>();
        rewritten_query->setTable(table.shortName);
        rewritten_query->setDatabase(mapped_to_database);
        if (drop_query.is_truncate)
            rewritten_query->kind = ASTDropQuery::Kind::Truncate;
        else
            rewritten_query->kind = ASTDropQuery::Kind::Drop;
        rewritten_query->is_view = false;
        //To avoid failure, we always set exists
        rewritten_query->if_exists = true;
        rewritten_querys.push_back(rewritten_query);
    }
    return rewritten_querys;
}

void InterpreterRenameImpl::validate(const InterpreterRenameImpl::TQuery & rename_query, ContextPtr /*context*/)
{
    if (rename_query.exchange)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot execute exchange for external ddl query.");
}

ASTs InterpreterRenameImpl::getRewrittenQueries(
    const InterpreterRenameImpl::TQuery & rename_query, ContextPtr context, const String & mapped_to_database, const String & mysql_database)
{
    ASTRenameQuery::Elements elements;
    for (const auto & rename_element : rename_query.getElements())
    {
        const auto & to_database = resolveDatabase(rename_element.to.getDatabase(), mysql_database, mapped_to_database, context);
        const auto & from_database = resolveDatabase(rename_element.from.getDatabase(), mysql_database, mapped_to_database, context);

        if ((from_database == mapped_to_database || to_database == mapped_to_database) && to_database != from_database)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot rename with other database for external ddl query.");

        if (from_database == mapped_to_database)
        {
            elements.push_back(ASTRenameQuery::Element());
            elements.back().from.database = std::make_shared<ASTIdentifier>(mapped_to_database);
            elements.back().from.table = rename_element.from.table->clone();
            elements.back().to.database = std::make_shared<ASTIdentifier>(mapped_to_database);
            elements.back().to.table = rename_element.to.table->clone();
        }
    }

    if (elements.empty())
        return ASTs{};

    auto rewritten_query = std::make_shared<ASTRenameQuery>(std::move(elements));
    return ASTs{rewritten_query};
}

void InterpreterAlterImpl::validate(const InterpreterAlterImpl::TQuery & /*query*/, ContextPtr /*context*/)
{
}

ASTs InterpreterAlterImpl::getRewrittenQueries(
    const InterpreterAlterImpl::TQuery & alter_query, ContextPtr context, const String & mapped_to_database, const String & mysql_database)
{
    if (resolveDatabase(alter_query.database, mysql_database, mapped_to_database, context) != mapped_to_database)
        return {};

    auto rewritten_alter_query = std::make_shared<ASTAlterQuery>();
    ASTRenameQuery::Elements rename_elements;

    rewritten_alter_query->setDatabase(mapped_to_database);
    rewritten_alter_query->setTable(alter_query.table);
    rewritten_alter_query->alter_object = ASTAlterQuery::AlterObjectType::TABLE;
    rewritten_alter_query->set(rewritten_alter_query->command_list, std::make_shared<ASTExpressionList>());

    String default_after_column;
    for (const auto & command_query : alter_query.command_list->children)
    {
        const auto & alter_command = command_query->as<MySQLParser::ASTAlterCommand>();

        if (alter_command->type == MySQLParser::ASTAlterCommand::ADD_COLUMN)
        {
            const auto & additional_columns_name_and_type = getColumnsList(alter_command->additional_columns);
            const auto & additional_columns_description = createColumnsDescription(additional_columns_name_and_type, alter_command->additional_columns);
            const auto & additional_columns = InterpreterCreateQuery::formatColumns(additional_columns_description);

            for (size_t index = 0; index < additional_columns_name_and_type.size(); ++index)
            {
                auto rewritten_command = std::make_shared<ASTAlterCommand>();
                rewritten_command->type = ASTAlterCommand::ADD_COLUMN;
                rewritten_command->first = alter_command->first;
                rewritten_command->col_decl = rewritten_command->children.emplace_back(additional_columns->children[index]->clone()).get();

                const auto & column_declare = alter_command->additional_columns->children[index]->as<MySQLParser::ASTDeclareColumn>();
                if (column_declare && column_declare->column_options)
                {
                    /// We need to add default expression for fill data
                    const auto & column_options = column_declare->column_options->as<MySQLParser::ASTDeclareOptions>();

                    const auto & default_expression_it = column_options->changes.find("default");
                    if (default_expression_it != column_options->changes.end())
                    {
                        ASTColumnDeclaration * col_decl = rewritten_command->col_decl->as<ASTColumnDeclaration>();
                        col_decl->default_specifier = "DEFAULT";
                        col_decl->default_expression = default_expression_it->second->clone();
                        col_decl->children.emplace_back(col_decl->default_expression);
                    }
                }

                if (default_after_column.empty())
                {
                    StoragePtr storage = DatabaseCatalog::instance().getTable({mapped_to_database, alter_query.table}, context);
                    Block storage_header = storage->getInMemoryMetadataPtr()->getSampleBlock();

                    /// Put the sign and version columns last
                    default_after_column = storage_header.getByPosition(storage_header.columns() - 3).name;
                }

                if (!alter_command->column_name.empty())
                {
                    rewritten_command->column = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(alter_command->column_name)).get();

                    /// For example(when add_column_1 is last column):
                    /// ALTER TABLE test_database.test_table_2 ADD COLUMN add_column_3 INT AFTER add_column_1, ADD COLUMN add_column_4 INT
                    /// In this case, we still need to change the default after column

                    if (alter_command->column_name == default_after_column)
                        default_after_column = rewritten_command->col_decl->as<ASTColumnDeclaration>()->name;
                }
                else
                {
                    rewritten_command->column = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(default_after_column)).get();
                    default_after_column = rewritten_command->col_decl->as<ASTColumnDeclaration>()->name;
                }

                rewritten_alter_query->command_list->children.push_back(rewritten_command);
            }
        }
        else if (alter_command->type == MySQLParser::ASTAlterCommand::DROP_COLUMN)
        {
            auto rewritten_command = std::make_shared<ASTAlterCommand>();
            rewritten_command->type = ASTAlterCommand::DROP_COLUMN;
            rewritten_command->column = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(alter_command->column_name)).get();
            rewritten_alter_query->command_list->children.push_back(rewritten_command);
        }
        else if (alter_command->type == MySQLParser::ASTAlterCommand::RENAME_COLUMN)
        {
            if (alter_command->old_name != alter_command->column_name)
            {
                /// 'RENAME column_name TO column_name' is not allowed in Clickhouse
                auto rewritten_command = std::make_shared<ASTAlterCommand>();
                rewritten_command->type = ASTAlterCommand::RENAME_COLUMN;
                rewritten_command->column = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(alter_command->old_name)).get();
                rewritten_command->rename_to = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(alter_command->column_name)).get();
                rewritten_alter_query->command_list->children.push_back(rewritten_command);
            }
        }
        else if (alter_command->type == MySQLParser::ASTAlterCommand::MODIFY_COLUMN)
        {
            String new_column_name;

            {
                auto rewritten_command = std::make_shared<ASTAlterCommand>();
                rewritten_command->type = ASTAlterCommand::MODIFY_COLUMN;
                rewritten_command->first = alter_command->first;
                auto modify_columns = getColumnsList(alter_command->additional_columns);

                if (modify_columns.size() != 1)
                    throw Exception(ErrorCodes::LOGICAL_ERROR, "It is a bug");

                new_column_name = modify_columns.front().name;

                if (!alter_command->old_name.empty())
                    modify_columns.front().name = alter_command->old_name;

                const auto & modify_columns_description = createColumnsDescription(modify_columns, alter_command->additional_columns);
                rewritten_command->col_decl = rewritten_command->children.emplace_back(InterpreterCreateQuery::formatColumns(modify_columns_description)->children[0]).get();

                if (!alter_command->column_name.empty())
                    rewritten_command->column = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(alter_command->column_name)).get();

                rewritten_alter_query->command_list->children.push_back(rewritten_command);
            }

            if (!alter_command->old_name.empty() && alter_command->old_name != new_column_name)
            {
                auto rewritten_command = std::make_shared<ASTAlterCommand>();
                rewritten_command->type = ASTAlterCommand::RENAME_COLUMN;
                rewritten_command->column = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(alter_command->old_name)).get();
                rewritten_command->rename_to = rewritten_command->children.emplace_back(std::make_shared<ASTIdentifier>(new_column_name)).get();
                rewritten_alter_query->command_list->children.push_back(rewritten_command);
            }
        }
        else if (alter_command->type == MySQLParser::ASTAlterCommand::RENAME_TABLE)
        {
            const auto & to_database = resolveDatabase(alter_command->new_database_name, mysql_database, mapped_to_database, context);

            if (to_database != mapped_to_database)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot rename with other database for external ddl query.");

            /// For ALTER TABLE table_name RENAME TO new_table_name_1, RENAME TO new_table_name_2;
            /// We just need to generate RENAME TABLE table_name TO new_table_name_2;
            if (rename_elements.empty())
                rename_elements.push_back(ASTRenameQuery::Element());

            rename_elements.back().from.database = std::make_shared<ASTIdentifier>(mapped_to_database);
            rename_elements.back().from.table = std::make_shared<ASTIdentifier>(alter_query.table);
            rename_elements.back().to.database = std::make_shared<ASTIdentifier>(mapped_to_database);
            rename_elements.back().to.table = std::make_shared<ASTIdentifier>(alter_command->new_table_name);
        }
    }

    ASTs rewritten_queries;

    /// Order is very important. We always execute alter first and then execute rename
    if (!rewritten_alter_query->command_list->children.empty())
        rewritten_queries.push_back(rewritten_alter_query);

    if (!rename_elements.empty())
    {
        auto rewritten_rename_query = std::make_shared<ASTRenameQuery>(std::move(rename_elements));
        rewritten_queries.push_back(rewritten_rename_query);
    }

    return rewritten_queries;
}

}

}
