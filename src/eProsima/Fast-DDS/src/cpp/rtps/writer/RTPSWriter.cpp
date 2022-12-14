// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * @file RTPSWriter.cpp
 *
 */

#include <fastdds/rtps/writer/RTPSWriter.h>

#include <fastdds/dds/log/Log.hpp>

#include <fastdds/rtps/history/WriterHistory.h>
#include <fastdds/rtps/messages/RTPSMessageCreator.h>

#include <rtps/history/BasicPayloadPool.hpp>
#include <rtps/history/CacheChangePool.h>
#include <rtps/flowcontrol/FlowController.h>
#include <rtps/participant/RTPSParticipantImpl.h>
#include <fstream>
#include <mutex>

std::string read_txt(std::string txt_file)
{
    std::ifstream ifs;
    ifs.open(txt_file, std::ios::in);
    if (!ifs.is_open())
    {
        std::cout << "文件打开失败" << std::endl;
        return "error";
    }
    std::string line;
    std::string data;
    while (std::getline(ifs, line))
    {
        data = line.c_str();
        std::cout << data << std::endl;
    }
    ifs.close();
    return data;
}

void write_txt(std::string txt_file, std::string data)
{
    std::ofstream ofs;
    ofs.open(txt_file, std::ios::out);
    ofs << data << std::endl;
    ofs.close();
}

int string_to_int(std::string s)
{
    // judege the string is int or not
    if (s.empty())
    {
        return 0;
    }
    if (s[0] == 'U')
    {
        return 0;
    }
    if (s[0] == 'e')
    {
        return 0;
    }
    int n = 0;

    for (int i = 0; i < s.length(); i++)
    {
        n = n * 10 + s[i] - '0';
    }
    return n;
}

namespace eprosima
{
    namespace fastrtps
    {
        namespace rtps
        {

            RTPSWriter::RTPSWriter(
                RTPSParticipantImpl *impl,
                const GUID_t &guid,
                const WriterAttributes &att,
                WriterHistory *hist,
                WriterListener *listen)
                : Endpoint(impl, guid, att.endpoint), mp_history(hist), mp_listener(listen), is_async_(att.mode == SYNCHRONOUS_WRITER ? false : true), locator_selector_(att.matched_readers_allocation), all_remote_readers_(att.matched_readers_allocation), all_remote_participants_(att.matched_readers_allocation), liveliness_kind_(att.liveliness_kind), liveliness_lease_duration_(att.liveliness_lease_duration), liveliness_announcement_period_(att.liveliness_announcement_period)
            {
                PoolConfig cfg = PoolConfig::from_history_attributes(hist->m_att);
                std::shared_ptr<IChangePool> change_pool;
                std::shared_ptr<IPayloadPool> payload_pool;
                payload_pool = BasicPayloadPool::get(cfg, change_pool);

                init(payload_pool, change_pool);
            }

            RTPSWriter::RTPSWriter(
                RTPSParticipantImpl *impl,
                const GUID_t &guid,
                const WriterAttributes &att,
                const std::shared_ptr<IPayloadPool> &payload_pool,
                WriterHistory *hist,
                WriterListener *listen)
                : RTPSWriter(
                      impl, guid, att, payload_pool,
                      std::make_shared<CacheChangePool>(PoolConfig::from_history_attributes(hist->m_att)),
                      hist, listen)
            {
            }

            RTPSWriter::RTPSWriter(
                RTPSParticipantImpl *impl,
                const GUID_t &guid,
                const WriterAttributes &att,
                const std::shared_ptr<IPayloadPool> &payload_pool,
                const std::shared_ptr<IChangePool> &change_pool,
                WriterHistory *hist,
                WriterListener *listen)
                : Endpoint(impl, guid, att.endpoint), mp_history(hist), mp_listener(listen), is_async_(att.mode == SYNCHRONOUS_WRITER ? false : true), locator_selector_(att.matched_readers_allocation), all_remote_readers_(att.matched_readers_allocation), all_remote_participants_(att.matched_readers_allocation), liveliness_kind_(att.liveliness_kind), liveliness_lease_duration_(att.liveliness_lease_duration), liveliness_announcement_period_(att.liveliness_announcement_period)
            {
                init(payload_pool, change_pool);
            }

            void RTPSWriter::init(
                const std::shared_ptr<IPayloadPool> &payload_pool,
                const std::shared_ptr<IChangePool> &change_pool)
            {
                payload_pool_ = payload_pool;
                change_pool_ = change_pool;
                fixed_payload_size_ = 0;
                if (mp_history->m_att.memoryPolicy == PREALLOCATED_MEMORY_MODE)
                {
                    fixed_payload_size_ = mp_history->m_att.payloadMaxSize;
                }

                mp_history->mp_writer = this;
                mp_history->mp_mutex = &mp_mutex;

                logInfo(RTPS_WRITER, "RTPSWriter created");
            }

            RTPSWriter::~RTPSWriter()
            {
                logInfo(RTPS_WRITER, "RTPSWriter destructor");

                // Deletion of the events has to be made in child destructor.

                for (auto it = mp_history->changesBegin(); it != mp_history->changesEnd(); ++it)
                {
                    release_change(*it);
                }

                mp_history->mp_writer = nullptr;
                mp_history->mp_mutex = nullptr;
            }

            CacheChange_t *RTPSWriter::new_change(
                const std::function<uint32_t()> &dataCdrSerializedSize,
                ChangeKind_t changeKind,
                InstanceHandle_t handle)
            {
                logInfo(RTPS_WRITER, "Creating new change");

                std::lock_guard<RecursiveTimedMutex> guard(mp_mutex);
                CacheChange_t *reserved_change = nullptr;
                if (!change_pool_->reserve_cache(reserved_change))
                {
                    logWarning(RTPS_WRITER, "Problem reserving cache from pool");
                    return nullptr;
                }

                uint32_t payload_size = fixed_payload_size_ ? fixed_payload_size_ : dataCdrSerializedSize();
                if (!payload_pool_->get_payload(payload_size, *reserved_change))
                {
                    change_pool_->release_cache(reserved_change);
                    logWarning(RTPS_WRITER, "Problem reserving payload from pool");
                    return nullptr;
                }

                reserved_change->kind = changeKind;
                if (m_att.topicKind == WITH_KEY && !handle.isDefined())
                {
                    logWarning(RTPS_WRITER, "Changes in KEYED Writers need a valid instanceHandle");
                }
                reserved_change->instanceHandle = handle;
                reserved_change->writerGUID = m_guid;
                return reserved_change;
            }

            bool RTPSWriter::release_change(
                CacheChange_t *change)
            {
                // Asserting preconditions
                assert(change != nullptr);
                assert(change->writerGUID == m_guid);

                std::lock_guard<RecursiveTimedMutex> guard(mp_mutex);

                IPayloadPool *pool = change->payload_owner();
                if (pool)
                {
                    pool->release_payload(*change);
                }
                return change_pool_->release_cache(change);
            }

            SequenceNumber_t RTPSWriter::get_seq_num_min()
            {
                CacheChange_t *change;
                if (mp_history->get_min_change(&change) && change != nullptr)
                {
                    return change->sequenceNumber;
                }
                else
                {
                    return c_SequenceNumber_Unknown;
                }
            }

            SequenceNumber_t RTPSWriter::get_seq_num_max()
            {
                CacheChange_t *change;
                if (mp_history->get_max_change(&change) && change != nullptr)
                {
                    return change->sequenceNumber;
                }
                else
                {
                    return c_SequenceNumber_Unknown;
                }
            }

            uint32_t RTPSWriter::getTypeMaxSerialized()
            {
                return mp_history->getTypeMaxSerialized();
            }

            bool RTPSWriter::remove_older_changes(
                unsigned int max)
            {
                logInfo(RTPS_WRITER, "Starting process clean_history for writer " << getGuid());
                std::lock_guard<RecursiveTimedMutex> guard(mp_mutex);
                bool limit = (max != 0);

                bool remove_ret = mp_history->remove_min_change();
                bool at_least_one = remove_ret;
                unsigned int count = 1;

                while (remove_ret && (!limit || count < max))
                {
                    remove_ret = mp_history->remove_min_change();
                    ++count;
                }

                return at_least_one;
            }

            CONSTEXPR uint32_t info_dst_message_length = 16;
            CONSTEXPR uint32_t info_ts_message_length = 12;
            CONSTEXPR uint32_t data_frag_submessage_header_length = 36;

            uint32_t RTPSWriter::getMaxDataSize()
            {
                return calculateMaxDataSize(mp_RTPSParticipant->getMaxMessageSize());
            }

            uint32_t RTPSWriter::calculateMaxDataSize(
                uint32_t length)
            {
                uint32_t maxDataSize = mp_RTPSParticipant->calculateMaxDataSize(length);

                maxDataSize -= info_dst_message_length +
                               info_ts_message_length +
                               data_frag_submessage_header_length;

                // TODO(Ricardo) inlineqos in future.

#if HAVE_SECURITY
                if (getAttributes().security_attributes().is_submessage_protected)
                {
                    maxDataSize -= mp_RTPSParticipant->security_manager().calculate_extra_size_for_rtps_submessage(m_guid);
                }

                if (getAttributes().security_attributes().is_payload_protected)
                {
                    maxDataSize -= mp_RTPSParticipant->security_manager().calculate_extra_size_for_encoded_payload(m_guid);
                }
#endif // if HAVE_SECURITY

                return maxDataSize;
            }

            void RTPSWriter::add_guid(
                const GUID_t &remote_guid)
            {
                const GuidPrefix_t &prefix = remote_guid.guidPrefix;
                all_remote_readers_.push_back(remote_guid);
                if (std::find(all_remote_participants_.begin(), all_remote_participants_.end(), prefix) ==
                    all_remote_participants_.end())
                {
                    all_remote_participants_.push_back(prefix);
                }
            }

            void RTPSWriter::compute_selected_guids()
            {
                all_remote_readers_.clear();
                all_remote_participants_.clear();

                for (LocatorSelectorEntry *entry : locator_selector_.transport_starts())
                {
                    GUID_t temp_id = entry->remote_guid;
                    std::string temp_topic_key = temp_id.instanceId;
                    int temp_topic_key_int = string_to_int(temp_topic_key);
                    // if (temp_topic_key_int > 20)
                    // {
                    //     if (temp_topic_key_int > 1000)
                    //     {
                    //         entry->enabled = false;
                    //         // entry->reset();
                    //         std::cout << "now_topic_key_int: " << temp_topic_key_int << std::endl;
                    //     }
                    // }
                    if (entry->enabled)
                    {
                        add_guid(entry->remote_guid);
                    }
                }
            }

            void RTPSWriter::update_cached_info_nts()
            {
                locator_selector_.reset(true);
                mp_RTPSParticipant->network_factory().select_locators(locator_selector_);
            }

            bool RTPSWriter::destinations_have_changed() const
            {
                return false;
            }

            GuidPrefix_t RTPSWriter::destination_guid_prefix() const
            {
                return all_remote_participants_.size() == 1 ? all_remote_participants_.at(0) : c_GuidPrefix_Unknown;
            }

            const std::vector<GuidPrefix_t> &RTPSWriter::remote_participants() const
            {
                return all_remote_participants_;
            }

            const std::vector<GUID_t> &RTPSWriter::remote_guids() const
            {
                return all_remote_readers_;
            }

            // judge whether the reader should be notified about the change
            bool Is_matched(
                const GUID_t &writer_guid,
                const GUID_t &reader_guid)
            {
                if (reader_guid.guidPrefix == c_GuidPrefix_Unknown)
                {
                    return true;
                }
                else if (reader_guid.guidPrefix == writer_guid.guidPrefix)
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }

            bool RTPSWriter::send(
                CDRMessage_t *message,
                std::chrono::steady_clock::time_point &max_blocking_time_point) const
            {
                RTPSParticipantImpl *participant = getRTPSParticipant();
                long locator_size = sizeof(locator_selector_.entries_);
                LocatorSelector locator_selector_temp = locator_selector_;
                LocatorSelector locator_selector_temp2(1);
                int write_flag = 0;
                int write_int = 0;
                if (locator_size > 0)
                {
                    for (LocatorSelectorEntry *entry : locator_selector_temp.entries_)
                    {
                        GUID_t temp_id = entry->remote_guid;
                        std::string temp_topic_key = temp_id.instanceId;
                        int temp_topic_key_int = string_to_int(temp_topic_key);
                        if (temp_topic_key_int > 20)
                        {

                            // if (temp_topic_key_int < 1000)
                            // {
                            std::cout << "temp_topic_key_int: " << temp_topic_key_int << std::endl;

                            std::string txt_file = "/home/nicsrobot/Fast-DDS/test.txt";
                            std::string now_index = read_txt(txt_file);
                            int now_int = string_to_int(now_index);
                            write_int = now_int;
                            // now_int < 10, topic_key == 9499 will be send
                            write_flag = 1;
                            if (now_int < 100)
                            {
                                if (temp_topic_key_int < 1000)
                                {
                                    // write_flag = 1;
                                    // entry->enabled = false;
                                    // entry->transport_should_process = false;
                                    // entry->reset();
                                    locator_selector_temp2.add_entry_tmp(entry, 0);

                                    std::cout << "now_int: " << now_int << std::endl;
                                    std::cout << "now_topic_key_int: " << temp_topic_key_int << std::endl;
                                }
                            }
                            if (now_int >= 100)
                            {
                                if (temp_topic_key_int > 1000)
                                {
                                    // write_flag = 1;
                                    // entry->enabled = false;
                                    // entry->transport_should_process = false;
                                    // entry->reset();
                                    locator_selector_temp2.add_entry_tmp(entry, 0);
                                    std::cout << "now_int: " << now_int << std::endl;
                                    std::cout << "now_topic_key_int: " << temp_topic_key_int << std::endl;
                                }
                            }
                            now_int++;
                            std::string then_index = std::to_string(now_int);
                            write_txt(txt_file, then_index);
                        }
                    }
                }

                locator_selector_temp2.fresh_selections();

                // 此处有golbal信息，可以在此添加筛选条件
                if (write_flag == 1)
                {
                    return locator_selector_temp2.selected_size() == 0 ||
                           // participant->sendSync(message, locator_selector_.begin(), locator_selector_.begin(), max_blocking_time_point);
                           participant->sendSync(message, locator_selector_temp2.begin(), locator_selector_temp2.end(), max_blocking_time_point);
                }
                else
                {
                    return locator_selector_temp.selected_size() == 0 ||
                           // participant->sendSync(message, locator_selector_.begin(), locator_selector_.begin(), max_blocking_time_point);
                           participant->sendSync(message, locator_selector_temp.begin(), locator_selector_temp.end(), max_blocking_time_point);
                }
            }

            const LivelinessQosPolicyKind &RTPSWriter::get_liveliness_kind() const
            {
                return liveliness_kind_;
            }

            const Duration_t &RTPSWriter::get_liveliness_lease_duration() const
            {
                return liveliness_lease_duration_;
            }

            const Duration_t &RTPSWriter::get_liveliness_announcement_period() const
            {
                return liveliness_announcement_period_;
            }

        } // namespace rtps
    }     // namespace fastrtps
} // namespace eprosima
