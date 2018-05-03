#include <Columns/ColumnWithDictionary.h>
#include <Columns/ColumnUnique.h>
#include <Columns/ColumnFixedString.h>
#include <Common/typeid_cast.h>
#include <Core/TypeListNumber.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeWithDictionary.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <Parsers/IAST.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int LOGICAL_ERROR;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

DataTypeWithDictionary::DataTypeWithDictionary(DataTypePtr dictionary_type_, DataTypePtr indexes_type_)
        : dictionary_type(std::move(dictionary_type_)), indexes_type(std::move(indexes_type_))
{
    if (!indexes_type->isUnsignedInteger())
        throw Exception("Index type of DataTypeWithDictionary must be unsigned integer, but got "
                        + indexes_type->getName(), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

    auto inner_type = dictionary_type;
    if (dictionary_type->isNullable())
        inner_type = static_cast<const DataTypeNullable &>(*dictionary_type).getNestedType();

    if (!inner_type->isStringOrFixedString()
        && !inner_type->isDateOrDateTime()
        && !inner_type->isNumber())
        throw Exception("DataTypeWithDictionary is supported only for numbers, strings, Date or DateTime, but got "
                        + dictionary_type->getName(), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
}

void DataTypeWithDictionary::enumerateStreams(StreamCallback callback, SubstreamPath path) const
{
    path.push_back(Substream::DictionaryElements);
    dictionary_type->enumerateStreams(callback, path);
    path.back() = Substream::DictionaryIndexes;
    indexes_type->enumerateStreams(callback, path);
}

void DataTypeWithDictionary::serializeBinaryBulkWithMultipleStreams(
        const IColumn & column,
        OutputStreamGetter getter,
        size_t offset,
        size_t limit,
        bool /*position_independent_encoding*/,
        SubstreamPath path) const
{
    const ColumnWithDictionary & column_with_dictionary = typeid_cast<const ColumnWithDictionary &>(column);

    path.push_back(Substream::DictionaryElements);
    if (auto stream = getter(path))
    {
        if (offset == 0)
        {
            auto nested = column_with_dictionary.getUnique()->getNestedColumn();
            UInt64 nested_size = nested->size();
            writeIntBinary(nested_size, *stream);
            dictionary_type->serializeBinaryBulk(*nested, *stream, 0, 0);
        }
    }

    path.back() = Substream::DictionaryIndexes;
    if (auto stream = getter(path))
        indexes_type->serializeBinaryBulk(*column_with_dictionary.getIndexes(), *stream, offset, limit);
}

void DataTypeWithDictionary::deserializeBinaryBulkWithMultipleStreams(
        IColumn & column,
        InputStreamGetter getter,
        size_t limit,
        double /*avg_value_size_hint*/,
        bool /*position_independent_encoding*/,
        SubstreamPath path) const
{
    ColumnWithDictionary & column_with_dictionary = typeid_cast<ColumnWithDictionary &>(column);

    path.push_back(Substream::DictionaryElements);
    if (ReadBuffer * stream = getter(path))
    {
        if (column.empty())
        {
            UInt64 nested_size;
            readIntBinary(nested_size, *stream);
            auto dict_column = column_with_dictionary.getUnique()->getNestedColumn()->cloneEmpty();
            dictionary_type->deserializeBinaryBulk(*dict_column, *stream, nested_size, 0);

            /// Note: it's assumed that rows inserted into columnUnique get incremental indexes.
            column_with_dictionary.getUnique()->uniqueInsertRangeFrom(*dict_column, 0, dict_column->size());
        }
    }

    path.back() = Substream::DictionaryIndexes;
    if (auto stream = getter(path))
        indexes_type->deserializeBinaryBulk(*column_with_dictionary.getIndexes(), *stream, limit, 0);
}

void DataTypeWithDictionary::serializeBinary(const Field & field, WriteBuffer & ostr) const
{
    dictionary_type->serializeBinary(field, ostr);
}
void DataTypeWithDictionary::deserializeBinary(Field & field, ReadBuffer & istr) const
{
    dictionary_type->deserializeBinary(field, istr);
}

template <typename ... Args>
void DataTypeWithDictionary::serializeImpl(
        const IColumn & column, size_t row_num, WriteBuffer & ostr,
        DataTypeWithDictionary::SerealizeFunctionPtr<Args ...> func, Args & ... args) const
{
    auto & column_with_dictionary = getColumnWithDictionary(column);
    size_t unique_row_number = column_with_dictionary.getIndexes()->getUInt(row_num);
    (dictionary_type.get()->*func)(*column_with_dictionary.getUnique(), unique_row_number, ostr, std::forward<Args>(args)...);
}

template <typename ... Args>
void DataTypeWithDictionary::deserializeImpl(
        IColumn & column, ReadBuffer & istr,
        DataTypeWithDictionary::DeserealizeFunctionPtr<Args ...> func, Args ... args) const
{
    auto & column_with_dictionary = getColumnWithDictionary(column);
    auto nested_unique = getNestedUniqueColumn(column_with_dictionary).assumeMutable();

    auto size = column_with_dictionary.size();
    auto unique_size = nested_unique->size();

    (dictionary_type.get()->*func)(*nested_unique, istr, std::forward<Args>(args)...);

    /// Note: Insertion into ColumnWithDictionary from it's nested column may cause insertion from column to itself.
    /// Generally it's wrong because column may reallocate memory before insertion.
    column_with_dictionary.insertFrom(*nested_unique, unique_size);
    if (column_with_dictionary.getIndexes()->getUInt(size) != unique_size)
        nested_unique->popBack(1);
}

template <typename ColumnType, typename IndexType>
MutableColumnPtr DataTypeWithDictionary::createColumnImpl() const
{
    return ColumnWithDictionary::create(ColumnUnique<ColumnType, IndexType>::create(dictionary_type),
                                        indexes_type->createColumn());
}

template <typename ColumnType>
MutableColumnPtr DataTypeWithDictionary::createColumnImpl() const
{
    if (typeid_cast<const DataTypeUInt8 *>(indexes_type.get()))
        return createColumnImpl<ColumnType, UInt8>();
    if (typeid_cast<const DataTypeUInt16 *>(indexes_type.get()))
        return createColumnImpl<ColumnType, UInt16>();
    if (typeid_cast<const DataTypeUInt32 *>(indexes_type.get()))
        return createColumnImpl<ColumnType, UInt32>();
    if (typeid_cast<const DataTypeUInt64 *>(indexes_type.get()))
        return createColumnImpl<ColumnType, UInt64>();

    throw Exception("The type of indexes must be unsigned integer, but got " + dictionary_type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
}

struct CreateColumnVector
{
    MutableColumnPtr & column;
    const DataTypeWithDictionary * data_type_with_dictionary;
    const IDataType * type;

    CreateColumnVector(MutableColumnPtr & column, const DataTypeWithDictionary * data_type_with_dictionary,
                       const IDataType * type)
            : column(column), data_type_with_dictionary(data_type_with_dictionary), type(type) {}

    template <typename T, size_t>
    void operator()()
    {
        if (typeid_cast<const DataTypeNumber<T> *>(type))
            column = data_type_with_dictionary->createColumnImpl<ColumnVector<T>>();
    }
};

MutableColumnPtr DataTypeWithDictionary::createColumn() const
{
    auto type = dictionary_type;
    if (type->isNullable())
        type = static_cast<const DataTypeNullable &>(*dictionary_type).getNestedType();

    if (type->isString())
        return createColumnImpl<ColumnString>();
    if (type->isFixedString())
        return createColumnImpl<ColumnFixedString>();
    if (typeid_cast<const DataTypeDate *>(type.get()))
        return createColumnImpl<ColumnVector<UInt16>>();
    if (typeid_cast<const DataTypeDateTime *>(type.get()))
        return createColumnImpl<ColumnVector<UInt32>>();
    if (type->isNumber())
    {
        MutableColumnPtr column;
        TypeListNumbers::forEach(CreateColumnVector(column, this, dictionary_type.get()));

        if (!column)
            throw Exception("Unexpected numeric type: " + type->getName(), ErrorCodes::LOGICAL_ERROR);

        return std::move(column);
    }

    throw Exception("Unexpected dictionary type for DataTypeWithDictionary: " + type->getName(),
                    ErrorCodes::LOGICAL_ERROR);
}

bool DataTypeWithDictionary::equals(const IDataType & rhs) const
{
    if (typeid(rhs) != typeid(*this))
        return false;

    auto & rhs_with_dictionary = static_cast<const DataTypeWithDictionary &>(rhs);
    return dictionary_type->equals(*rhs_with_dictionary.dictionary_type)
           && indexes_type->equals(*rhs_with_dictionary.indexes_type);
}



static DataTypePtr create(const ASTPtr & arguments)
{
    if (!arguments || arguments->children.size() != 2)
        throw Exception("WithDictionary data type family must have two arguments - type of elements and type of indices"
                        , ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

    return std::make_shared<DataTypeWithDictionary>(DataTypeFactory::instance().get(arguments->children[0]),
                                                    DataTypeFactory::instance().get(arguments->children[1]));
}

void registerDataTypeWithDictionary(DataTypeFactory & factory)
{
    factory.registerDataType("WithDictionary", create);
}

}