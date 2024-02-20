#include <Poco/File.h>

#include <Common/Exception.h>
#include <Common/IO/WriteHelpers.h>

#include <Service/KeeperUtils.h>
#include <Service/ReadBufferFromNuRaftBuffer.h>
#include <Service/SnapshotCommon.h>
#include <Service/WriteBufferFromNuraftBuffer.h>
#include <ZooKeeper/ZooKeeperIO.h>

#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif

namespace RK
{

using nuraft::cs_new;

String toString(SnapshotVersion version)
{
    switch (version)
    {
        case SnapshotVersion::V0:
            return "v0";
        case SnapshotVersion::V1:
            return "v1";
        case SnapshotVersion::V2:
            return "v2";
        case SnapshotVersion::V3:
            return "v3";
        case SnapshotVersion::None:
            return "none";
    }
}

int openFileForWrite(const String & obj_path)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));
    errno = 0;
    int snap_fd = ::open(obj_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (snap_fd < 0)
    {
        LOG_ERROR(log, "Created new snapshot object {} failed, fd {}, error:{}", obj_path, snap_fd, strerror(errno));
        return -1;
    }
    return snap_fd;
}

bool isFileHeader(UInt64 magic)
{
    union
    {
        uint64_t magic_num;
        uint8_t magic_array[8] = {'S', 'n', 'a', 'p', 'H', 'e', 'a', 'd'};
    };
    return magic == magic_num;
}

bool isFileTail(UInt64 magic)
{
    union
    {
        uint64_t magic_num;
        uint8_t magic_array[8] = {'S', 'n', 'a', 'p', 'T', 'a', 'i', 'l'};
    };
    return magic == magic_num;
}

std::shared_ptr<WriteBufferFromFile> openFileAndWriteHeader(const String & path, const SnapshotVersion version)
{
    auto out = std::make_shared<WriteBufferFromFile>(path);
    out->write(MAGIC_SNAPSHOT_HEAD.data(), MAGIC_SNAPSHOT_HEAD.size());
    writeIntBinary(static_cast<uint8_t>(version), *out);
    return out;
}

void writeTailAndClose(std::shared_ptr<WriteBufferFromFile> & out, UInt32 checksum)
{
    out->write(MAGIC_SNAPSHOT_TAIL.data(), MAGIC_SNAPSHOT_TAIL.size());
    writeIntBinary(checksum, *out);
    out->close();
}

int openFileForRead(String & obj_path)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));
    errno = 0;
    int snap_fd = ::open(obj_path.c_str(), O_RDWR);
    if (snap_fd < 0)
    {
        LOG_ERROR(log, "Open snapshot object {} failed, fd {}, error:{}", obj_path, snap_fd, strerror(errno));
        return -1;
    }
    return snap_fd;
}


/// save batch data in snapshot object
std::pair<size_t, UInt32> saveBatch(std::shared_ptr<WriteBufferFromFile> & out, ptr<SnapshotBatchPB> & batch)
{
    if (!batch)
        batch = cs_new<SnapshotBatchPB>();

    String str_buf;
    batch->SerializeToString(&str_buf);

    SnapshotBatchHeader header;
    header.data_length = str_buf.size();
    header.data_crc = RK::getCRC32(str_buf.c_str(), str_buf.size());

    writeIntBinary(header.data_length, *out);
    writeIntBinary(header.data_crc, *out);

    out->write(str_buf.c_str(), header.data_length);
    out->next();

    return {SnapshotBatchHeader::HEADER_SIZE + header.data_length, header.data_crc};
}

std::pair<size_t, UInt32>
saveBatchAndUpdateCheckSum(std::shared_ptr<WriteBufferFromFile> & out, ptr<SnapshotBatchPB> & batch, UInt32 checksum)
{
    auto [save_size, data_crc] = saveBatch(out, batch);
    /// rebuild batch
    batch = cs_new<SnapshotBatchPB>();
    return {save_size, updateCheckSum(checksum, data_crc)};
}

void serializeAcls(ACLMap & acls, String path, UInt32 save_batch_size, SnapshotVersion version)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));

    const auto & acl_map = acls.getMapping();
    LOG_INFO(log, "Begin create snapshot acl object, acl size {}, path {}", acl_map.size(), path);

    auto out = openFileAndWriteHeader(path, version);
    ptr<SnapshotBatchPB> batch;

    uint64_t index = 0;
    UInt32 checksum = 0;

    for (const auto & acl_it : acl_map)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                auto [save_size, new_checksum] = saveBatchAndUpdateCheckSum(out, batch, checksum);
                checksum = new_checksum;
            }
            batch = cs_new<SnapshotBatchPB>();
            batch->set_batch_type(SnapshotTypePB::SNAPSHOT_TYPE_ACLMAP);
        }

        /// append to batch
        SnapshotItemPB * entry = batch->add_data();
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(acl_it.first, buf);
        Coordination::write(acl_it.second, buf);

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        entry->set_data(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last acl batch
    auto [_, new_checksum] = saveBatchAndUpdateCheckSum(out, batch, checksum);
    checksum = new_checksum;

    writeTailAndClose(out, checksum);
}

[[maybe_unused]] size_t serializeEphemerals(KeeperStore::Ephemerals & ephemerals, std::mutex & mutex, String path, UInt32 save_batch_size)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));
    LOG_INFO(log, "Begin create snapshot ephemeral object, node size {}, path {}", ephemerals.size(), path);

    ptr<SnapshotBatchPB> batch;

    std::lock_guard lock(mutex);

    if (ephemerals.empty())
    {
        LOG_INFO(log, "Create snapshot ephemeral nodes size is 0");
        return 0;
    }

    auto out = cs_new<WriteBufferFromFile>(path);
    uint64_t index = 0;
    for (auto & ephemeral_it : ephemerals)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                saveBatch(out, batch);
            }
            batch = cs_new<SnapshotBatchPB>();
            batch->set_batch_type(SnapshotTypePB::SNAPSHOT_TYPE_DATA_EPHEMERAL);
        }

        /// append to batch
        SnapshotItemPB * entry = batch->add_data();
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(ephemeral_it.first, buf);
        Coordination::write(ephemeral_it.second.size(), buf);

        for (const auto & node_path : ephemeral_it.second)
        {
            Coordination::write(node_path, buf);
        }

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        entry->set_data(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last batch
    saveBatch(out, batch);
    out->close();
    return 1;
}

/**
 * Serialize sessions and return the next_session_id before serialize
 */
int64_t serializeSessions(KeeperStore & store, UInt32 save_batch_size, const SnapshotVersion version, String & path)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));

    auto out = openFileAndWriteHeader(path, version);


    LOG_INFO(log, "Begin create snapshot session object, session size {}, path {}", store.session_and_timeout.size(), path);

    std::lock_guard lock(store.session_mutex);
    std::lock_guard acl_lock(store.auth_mutex);

    int64_t next_session_id = store.session_id_counter;
    ptr<SnapshotBatchPB> batch;

    uint64_t index = 0;
    UInt32 checksum = 0;

    for (auto & session_it : store.session_and_timeout)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                auto [save_size, new_checksum] = saveBatchAndUpdateCheckSum(out, batch, checksum);
                checksum = new_checksum;
            }
            batch = cs_new<SnapshotBatchPB>();
            batch->set_batch_type(SnapshotTypePB::SNAPSHOT_TYPE_SESSION);
        }

        /// append to batch
        SnapshotItemPB * entry = batch->add_data();
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(session_it.first, buf); //NewSession
        Coordination::write(session_it.second, buf); //Timeout_ms

        Coordination::AuthIDs ids;
        if (store.session_and_auth.count(session_it.first))
            ids = store.session_and_auth.at(session_it.first);
        Coordination::write(ids, buf);

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        entry->set_data(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last batch
    auto [_, new_checksum] = saveBatchAndUpdateCheckSum(out, batch, checksum);
    checksum = new_checksum;
    writeTailAndClose(out, checksum);

    return next_session_id;
}

/**
 * Save map<string, string> or map<string, uint64>
 */
template <typename T>
void serializeMap(T & snap_map, UInt32 save_batch_size, SnapshotVersion version, String & path)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));
    LOG_INFO(log, "Begin create snapshot map object, map size {}, path {}", snap_map.size(), path);

    auto out = openFileAndWriteHeader(path, version);
    ptr<SnapshotBatchPB> batch;

    uint64_t index = 0;
    UInt32 checksum = 0;

    for (auto & it : snap_map)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                auto [save_size, new_checksum] = saveBatchAndUpdateCheckSum(out, batch, checksum);
                checksum = new_checksum;
            }

            batch = cs_new<SnapshotBatchPB>();
            if constexpr (std::is_same_v<T, StringMap>)
                batch->set_batch_type(SnapshotTypePB::SNAPSHOT_TYPE_STRINGMAP);
            else if constexpr (std::is_same_v<T, IntMap>)
                batch->set_batch_type(SnapshotTypePB::SNAPSHOT_TYPE_UINTMAP);
            else
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Only support string and int map.");
        }

        /// append to batch
        SnapshotItemPB * entry = batch->add_data();
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(it.first, buf);
        Coordination::write(it.second, buf);

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        entry->set_data(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last batch
    auto [_, new_checksum] = saveBatchAndUpdateCheckSum(out, batch, checksum);
    checksum = new_checksum;
    writeTailAndClose(out, checksum);
}

template void serializeMap<StringMap>(StringMap & snap_map, UInt32 save_batch_size, SnapshotVersion version, String & path);
template void serializeMap<IntMap>(IntMap & snap_map, UInt32 save_batch_size, SnapshotVersion version, String & path);

std::pair<size_t, UInt32> saveBatchV2(std::shared_ptr<WriteBufferFromFile> & out, ptr<SnapshotBatchBody> & batch)
{
    if (!batch)
        batch = cs_new<SnapshotBatchBody>();

    String str_buf = SnapshotBatchBody::serialize(*batch);

    SnapshotBatchHeader header;
    header.data_length = str_buf.size();
    header.data_crc = RK::getCRC32(str_buf.c_str(), str_buf.size());

    writeIntBinary(header.data_length, *out);
    writeIntBinary(header.data_crc, *out);

    out->write(str_buf.c_str(), header.data_length);
    out->next();

    return {SnapshotBatchHeader::HEADER_SIZE + header.data_length, header.data_crc};
}

UInt32 updateCheckSum(UInt32 checksum, UInt32 data_crc)
{
    union
    {
        UInt64 data;
        UInt32 crc[2];
    };
    crc[0] = checksum;
    crc[1] = data_crc;
    return RK::getCRC32(reinterpret_cast<const char *>(&data), 8);
}

std::pair<size_t, UInt32>
saveBatchAndUpdateCheckSumV2(std::shared_ptr<WriteBufferFromFile> & out, ptr<SnapshotBatchBody> & batch, UInt32 checksum)
{
    auto [save_size, data_crc] = saveBatchV2(out, batch);
    /// rebuild batch
    batch = cs_new<SnapshotBatchBody>();
    return {save_size, updateCheckSum(checksum, data_crc)};
}

void serializeAclsV2(ACLMap & acls, String path, UInt32 save_batch_size, SnapshotVersion version)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));

    const auto & acl_map = acls.getMapping();
    LOG_INFO(log, "Begin create snapshot acl object, acl size {}, path {}", acl_map.size(), path);

    auto out = openFileAndWriteHeader(path, version);
    ptr<SnapshotBatchBody> batch;

    uint64_t index = 0;
    UInt32 checksum = 0;

    for (const auto & acl_it : acl_map)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                auto [save_size, new_checksum] = saveBatchAndUpdateCheckSumV2(out, batch, checksum);
                checksum = new_checksum;
            }
            batch = cs_new<SnapshotBatchBody>();
            batch->type = SnapshotBatchType::SNAPSHOT_TYPE_ACLMAP;
        }

        /// append to batch
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(acl_it.first, buf);
        Coordination::write(acl_it.second, buf);

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        batch->add(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last acl batch
    auto [_, new_checksum] = saveBatchAndUpdateCheckSumV2(out, batch, checksum);
    checksum = new_checksum;

    writeTailAndClose(out, checksum);
}

[[maybe_unused]] size_t serializeEphemeralsV2(KeeperStore::Ephemerals & ephemerals, std::mutex & mutex, String path, UInt32 save_batch_size)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));
    LOG_INFO(log, "Begin create snapshot ephemeral object, node size {}, path {}", ephemerals.size(), path);

    ptr<SnapshotBatchBody> batch;

    std::lock_guard lock(mutex);

    if (ephemerals.empty())
    {
        LOG_INFO(log, "Create snapshot ephemeral nodes size is 0");
        return 0;
    }

    auto out = cs_new<WriteBufferFromFile>(path);
    uint64_t index = 0;
    for (auto & ephemeral_it : ephemerals)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                saveBatchV2(out, batch);
            }
            batch = cs_new<SnapshotBatchBody>();
            batch->type = SnapshotBatchType::SNAPSHOT_TYPE_DATA_EPHEMERAL;
        }

        /// append to batch
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(ephemeral_it.first, buf);
        Coordination::write(ephemeral_it.second.size(), buf);

        for (const auto & node_path : ephemeral_it.second)
        {
            Coordination::write(node_path, buf);
        }

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        batch->add(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last batch
    saveBatchV2(out, batch);
    out->close();
    return 1;
}

int64_t serializeSessionsV2(KeeperStore & store, UInt32 save_batch_size, const SnapshotVersion version, String & path)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));

    auto out = openFileAndWriteHeader(path, version);


    LOG_INFO(log, "Begin create snapshot session object, session size {}, path {}", store.session_and_timeout.size(), path);

    std::lock_guard lock(store.session_mutex);
    std::lock_guard acl_lock(store.auth_mutex);

    int64_t next_session_id = store.session_id_counter;
    ptr<SnapshotBatchBody> batch;

    uint64_t index = 0;
    UInt32 checksum = 0;

    for (auto & session_it : store.session_and_timeout)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                auto [save_size, new_checksum] = saveBatchAndUpdateCheckSumV2(out, batch, checksum);
                checksum = new_checksum;
            }
            batch = cs_new<SnapshotBatchBody>();
            batch->type = SnapshotBatchType::SNAPSHOT_TYPE_SESSION;
        }

        /// append to batch
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(session_it.first, buf); //NewSession
        Coordination::write(session_it.second, buf); //Timeout_ms

        Coordination::AuthIDs ids;
        if (store.session_and_auth.count(session_it.first))
            ids = store.session_and_auth.at(session_it.first);
        Coordination::write(ids, buf);

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        batch->add(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last batch
    auto [_, new_checksum] = saveBatchAndUpdateCheckSumV2(out, batch, checksum);
    checksum = new_checksum;
    writeTailAndClose(out, checksum);

    return next_session_id;
}

template <typename T>
void serializeMapV2(T & snap_map, UInt32 save_batch_size, SnapshotVersion version, String & path)
{
    Poco::Logger * log = &(Poco::Logger::get("KeeperSnapshotStore"));
    LOG_INFO(log, "Begin create snapshot map object, map size {}, path {}", snap_map.size(), path);

    auto out = openFileAndWriteHeader(path, version);
    ptr<SnapshotBatchBody> batch;

    uint64_t index = 0;
    UInt32 checksum = 0;

    for (auto & it : snap_map)
    {
        /// flush and rebuild batch
        if (index % save_batch_size == 0)
        {
            /// skip flush the first batch
            if (index != 0)
            {
                /// write data in batch to file
                auto [save_size, new_checksum] = saveBatchAndUpdateCheckSumV2(out, batch, checksum);
                checksum = new_checksum;
            }

            batch = cs_new<SnapshotBatchBody>();
            if constexpr (std::is_same_v<T, StringMap>)
                batch->type = SnapshotBatchType::SNAPSHOT_TYPE_STRINGMAP;
            else if constexpr (std::is_same_v<T, IntMap>)
                batch->type = SnapshotBatchType::SNAPSHOT_TYPE_UINTMAP;
            else
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Only support string and int map.");
        }

        /// append to batch
        WriteBufferFromNuraftBuffer buf;
        Coordination::write(it.first, buf);
        Coordination::write(it.second, buf);

        ptr<buffer> data = buf.getBuffer();
        data->pos(0);
        batch->add(String(reinterpret_cast<char *>(data->data_begin()), data->size()));

        index++;
    }

    /// flush the last batch
    auto [_, new_checksum] = saveBatchAndUpdateCheckSumV2(out, batch, checksum);
    checksum = new_checksum;
    writeTailAndClose(out, checksum);
}

template void serializeMapV2<StringMap>(StringMap & snap_map, UInt32 save_batch_size, SnapshotVersion version, String & path);
template void serializeMapV2<IntMap>(IntMap & snap_map, UInt32 save_batch_size, SnapshotVersion version, String & path);

void SnapshotBatchBody::add(const String & element)
{
    elements.push_back(element);
}

size_t SnapshotBatchBody::size() const
{
    return elements.size();
}

String & SnapshotBatchBody::operator[](size_t n)
{
    return elements.at(n);
}

String SnapshotBatchBody::serialize(const SnapshotBatchBody & batch_body)
{
    WriteBufferFromOwnString buf;
    writeIntBinary(static_cast<int32_t>(batch_body.type), buf);
    writeIntBinary(static_cast<int32_t>(batch_body.elements.size()), buf);
    for (const auto & element : batch_body.elements)
    {
        writeIntBinary(static_cast<int32_t>(element.size()), buf);
        writeString(element, buf);
    }
    return std::move(buf.str());
}

ptr<SnapshotBatchBody> SnapshotBatchBody::parse(const String & data)
{
    ptr<SnapshotBatchBody> batch_body = std::make_shared<SnapshotBatchBody>();
    ReadBufferFromMemory in(data.c_str(), data.size());
    int32_t type;
    readIntBinary(type, in);
    batch_body->type = static_cast<SnapshotBatchType>(type);
    int32_t element_count;
    readIntBinary(element_count, in);
    batch_body->elements.reserve(element_count);
    for (int i = 0; i < element_count; i++)
    {
        int32_t element_size;
        readIntBinary(element_size, in);
        String element;
        element.resize(element_size);
        in.readStrict(element.data(), element_size);
        batch_body->elements.emplace_back(std::move(element));
    }
    return batch_body;
}

}

#ifdef __clang__
#    pragma clang diagnostic pop
#endif