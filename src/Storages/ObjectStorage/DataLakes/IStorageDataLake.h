#pragma once

#include "config.h"

#if USE_AVRO

#include <Storages/IStorage.h>
#include <Storages/StorageFactory.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/DataLakes/IDataLakeMetadata.h>
#include <Storages/ObjectStorage/DataLakes/IcebergMetadata.h>
#include <Storages/ObjectStorage/DataLakes/HudiMetadata.h>
#include <Storages/ObjectStorage/DataLakes/DeltaLakeMetadata.h>
#include <Common/logger_useful.h>


namespace DB
{

namespace ErrorCodes
{
extern const int FORMAT_VERSION_TOO_OLD;
extern const int NOT_IMPLEMENTED;
}

/// Storage for read-only integration with Apache Iceberg tables in Amazon S3 (see https://iceberg.apache.org/)
/// Right now it's implemented on top of StorageS3 and right now it doesn't support
/// many Iceberg features like schema evolution, partitioning, positional and equality deletes.
template <typename DataLakeMetadata>
class IStorageDataLake final : public StorageObjectStorage
{
public:
    using Storage = StorageObjectStorage;
    using ConfigurationPtr = Storage::ConfigurationPtr;

    static StoragePtr create(
        ConfigurationPtr base_configuration,
        ContextPtr context,
        const StorageID & table_id_,
        const ColumnsDescription & columns_,
        const ConstraintsDescription & constraints_,
        const String & comment_,
        std::optional<FormatSettings> format_settings_,
        LoadingStrictnessLevel mode)
    {
        auto object_storage = base_configuration->createObjectStorage(context, /* is_readonly */ true);
        NamesAndTypesList schema_from_metadata;
        const bool use_schema_from_metadata = columns_.empty();

        if (base_configuration->format == "auto")
            base_configuration->format = "Parquet";

        ConfigurationPtr configuration = base_configuration->clone();

        DataLakeMetadataPtr datalake_metadata;

        try
        {
            datalake_metadata = DataLakeMetadata::create(object_storage, base_configuration, context);
            configuration->setPaths(datalake_metadata->getDataFileInfos(nullptr));
            if (use_schema_from_metadata)
                schema_from_metadata = datalake_metadata->getTableSchema();
        }
        catch (...)
        {
            if (mode <= LoadingStrictnessLevel::CREATE)
                throw;

            datalake_metadata.reset();
            configuration->setPaths({});
            tryLogCurrentException(__PRETTY_FUNCTION__);
        }

        return std::make_shared<IStorageDataLake<DataLakeMetadata>>(
            base_configuration,
            std::move(datalake_metadata),
            configuration,
            object_storage,
            context,
            table_id_,
            use_schema_from_metadata ? ColumnsDescription(schema_from_metadata) : columns_,
            constraints_,
            comment_,
            format_settings_);
    }

    String getName() const override { return DataLakeMetadata::name; }

    static ColumnsDescription getTableStructureFromData(
        ObjectStoragePtr object_storage_,
        ConfigurationPtr base_configuration,
        const std::optional<FormatSettings> & format_settings_,
        ContextPtr local_context)
    {
        auto metadata = DataLakeMetadata::create(object_storage_, base_configuration, local_context);

        auto schema_from_metadata = metadata->getTableSchema();
        if (!schema_from_metadata.empty())
        {
            return ColumnsDescription(std::move(schema_from_metadata));
        }

        ConfigurationPtr configuration = base_configuration->clone();
        configuration->setPaths(metadata->getDataFiles());
        std::string sample_path;
        return Storage::resolveSchemaFromData(object_storage_, configuration, format_settings_, sample_path, local_context);
    }

    void updateConfiguration(ContextPtr local_context) override
    {
        Storage::updateConfiguration(local_context);

        auto new_metadata = DataLakeMetadata::create(Storage::object_storage, base_configuration, local_context);
        if (!current_metadata || (*current_metadata != *new_metadata))
        {
            if constexpr (std::is_same_v<IcebergMetadata, DataLakeMetadata>)
            {
                throw Exception(
                    ErrorCodes::FORMAT_VERSION_TOO_OLD,
                    "Storage thinks that actual metadata version is {}, but actual metadata version is {} current",
                    (dynamic_cast<IcebergMetadata *>(current_metadata.get()) != nullptr)
                        ? dynamic_cast<IcebergMetadata *>(current_metadata.get())->getVersion()
                        : -1,
                    (dynamic_cast<IcebergMetadata *>(new_metadata.get()) != nullptr)
                        ? dynamic_cast<IcebergMetadata *>(new_metadata.get())->getVersion()
                        : -1);
            }
            else
            {
                current_metadata = std::move(new_metadata);
            }
        }

        auto updated_configuration = base_configuration->clone();
        updated_configuration->setPaths(current_metadata->getDataFileInfos(nullptr));
        updated_configuration->setPartitionColumns(current_metadata->getPartitionColumns());

        Storage::configuration = updated_configuration;
    }

    void refreshFilesWithFilterDag(const ActionsDAG & filter_dag) override
    {
        LOG_DEBUG(
            &Poco::Logger::get("Refresh called"),
            "It is Iceberg: {}",
            static_cast<bool>(std::is_same_v<IcebergMetadata, DataLakeMetadata>));

        if constexpr (std::is_same_v<IcebergMetadata, DataLakeMetadata>)
        {
            LOG_DEBUG(&Poco::Logger::get("getDataFileInfos with not null filter dag is called"), "");
            configuration->setPaths(current_metadata->getDataFileInfos(&filter_dag));
        }
    }

    template <typename... Args>
    IStorageDataLake(
        ConfigurationPtr base_configuration_,
        DataLakeMetadataPtr metadata_,
        Args &&... args)
        : Storage(std::forward<Args>(args)...)
        , base_configuration(base_configuration_)
        , current_metadata(std::move(metadata_))
    {
        if (base_configuration->format == "auto")
        {
            base_configuration->format = Storage::configuration->format;
        }

        if (current_metadata)
        {
            const auto & columns = current_metadata->getPartitionColumns();
            base_configuration->setPartitionColumns(columns);
            Storage::configuration->setPartitionColumns(columns);
        }
    }

    void updateExternalDynamicMetadata(ContextPtr context) override
    {
        if constexpr (std::is_same_v<DataLakeMetadata, IcebergMetadata>)
        {
            current_metadata = DataLakeMetadata::create(Storage::object_storage, base_configuration, context);
            ColumnsDescription column_description{current_metadata->getTableSchema()};
            StorageInMemoryMetadata metadata;
            metadata.setColumns(std::move(column_description));
            setInMemoryMetadata(metadata);
            return;
        }
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method updateExternalDynamicMetadata is not supported by storage {}", getName());
    }

    bool hasExternalDynamicMetadata() const override { return std::is_same_v<DataLakeMetadata, IcebergMetadata>; }

private:
    ConfigurationPtr base_configuration;
    DataLakeMetadataPtr current_metadata;

    ReadFromFormatInfo prepareReadingFromFormat(
        const Strings & requested_columns,
        const StorageSnapshotPtr & storage_snapshot,
        bool supports_subset_of_columns,
        ContextPtr local_context) override
    {
        auto info = DB::prepareReadingFromFormat(requested_columns, storage_snapshot, local_context, supports_subset_of_columns);
        if (!current_metadata)
        {
            Storage::updateConfiguration(local_context);
            current_metadata = DataLakeMetadata::create(Storage::object_storage, base_configuration, local_context);
        }
        auto column_mapping = current_metadata->getColumnNameToPhysicalNameMapping();
        if (!column_mapping.empty())
        {
            for (const auto & [column_name, physical_name] : column_mapping)
            {
                auto & column = info.format_header.getByName(column_name);
                column.name = physical_name;
            }
        }
        return info;
    }
};

using StorageIceberg = IStorageDataLake<IcebergMetadata>;
using StorageDeltaLake = IStorageDataLake<DeltaLakeMetadata>;
using StorageHudi = IStorageDataLake<HudiMetadata>;

}

#endif
