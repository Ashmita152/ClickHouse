#pragma once
#include <ostream>
#include <Processors/Formats/Impl/Parquet/ColumnFilter.h>
#include <DataTypes/IDataType.h>

namespace parquet
{
    class ColumnDescriptor;
    namespace schema
    {
        class Node;
        using NodePtr = std::shared_ptr<Node>;
    }
}

namespace DB
{
class SelectiveColumnReader;
using SelectiveColumnReaderPtr = std::shared_ptr<SelectiveColumnReader>;
class LazyPageReader;
struct RowGroupContext;

using PageReaderCreator = std::function<std::unique_ptr<LazyPageReader>()>;

class ParquetColumnReaderFactory
{
public:
    class Builder
    {
    public:
        Builder& dictionary(bool dictionary);
        Builder& nullable(bool nullable);
        Builder& columnDescriptor(const parquet::ColumnDescriptor * columnDescr);
        Builder& filter(const ColumnFilterPtr & filter);
        Builder& targetType(const DataTypePtr & target_type);
        Builder& pageReader(PageReaderCreator page_reader_creator);
        SelectiveColumnReaderPtr build();
    private:
        bool dictionary_ = false;
        bool nullable_ = false;
        const parquet::ColumnDescriptor * column_descriptor_ = nullptr;
        DataTypePtr target_type_ = nullptr;
        PageReaderCreator page_reader_creator = nullptr;
        std::unique_ptr<LazyPageReader> page_reader_ = nullptr;
        ColumnFilterPtr filter_ = nullptr;
    };

    static Builder builder();
};

SelectiveColumnReaderPtr createColumnReaderRecursive(const RowGroupContext& context, parquet::schema::NodePtr node, int def_level, int rep_level, bool condition_column, const ColumnFilterPtr & filter, const DataTypePtr & target_type);

}
