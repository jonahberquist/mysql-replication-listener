/*
Copyright (c) 2003, 2011, 2013, Oracle and/or its affiliates. All rights
reserved.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of
the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
02110-1301  USA
*/
#include "protocol.h"
#include "transitional_methods.h"
#include "binlog_api.h"
#include <my_global.h>
#include <mysql_com.h>
#include <iostream>
#include <stdint.h>
#include <vector>

using namespace mysql;
using namespace mysql::system;
using namespace std;
namespace mysql { namespace system {



/**
  Checks the Format Description event to determine if the master
  has binlog checksums enabled or not.
*/
int check_checksum_value(mysql::Binary_log_event **event)
{

  Format_event *fdev= static_cast<Format_event*>(*event);

  uchar version_split[3];
  do_server_version_split((fdev->master_version).c_str(), version_split);
  if (version_product(version_split) >= checksum_version_product)
  {
    /*
      Last four bytes is the check sum value which is to be removed
      from post_header_len.
    */
    fdev->post_header_len.erase(fdev->post_header_len.end() -
                                BINLOG_CHECKSUM_LEN,
                                fdev->post_header_len.end());

    // Last element in post_header_len is the checksum algorithm descriptor.
    if ((int)fdev->post_header_len.back() ==
        mysql::system::BINLOG_CHECKSUM_ALG_CRC32)
      return mysql::ERR_CHECKSUM_ENABLED;
  }
  return mysql::ERR_OK;
}


int proto_get_one_package(MYSQL *mysql, char *buff,
                           uint8_t *packet_no)
{
  ulong packet_length;
  packet_length= cli_safe_read(mysql);
  buff= (char*)mysql->net.buff;
  return packet_length;
}

void prot_parse_error_message(std::istream &is, struct st_error_package &err,
                              int packet_length)
{
  uint8_t marker;

  Protocol_chunk<uint16_t> prot_errno(err.error_code);
  Protocol_chunk<uint8_t>  prot_marker(marker);
  Protocol_chunk<uint8_t>  prot_sql_state(err.sql_state,5);

  is >> prot_errno
     >> prot_marker
     >> prot_sql_state;

    // TODO is the number of bytes read = is.tellg() ?

  int message_size= packet_length -2 -1 -5; // the remaining part of the package
  Protocol_chunk_string prot_message(err.message, message_size);
  is >> prot_message;
  err.message[message_size]= '\0';
}

void prot_parse_ok_message(std::istream &is, struct st_ok_package &ok,
                           int packet_length)
{
 // TODO: Assure that zero length messages can be but on the input stream.

  //Protocol_chunk<uint8_t>  prot_result_type(result_type);
  Protocol_chunk<uint64_t> prot_affected_rows(ok.affected_rows);
  Protocol_chunk<uint64_t> prot_insert_id(ok.insert_id);
  Protocol_chunk<uint16_t> prot_server_status(ok.server_status);
  Protocol_chunk<uint16_t> prot_warning_count(ok.warning_count);

  int message_size= packet_length -2  -prot_affected_rows.size()
                    -prot_insert_id.size() -prot_server_status.size()
                    -prot_warning_count.size();

  prot_affected_rows.set_length_encoded_binary(true);
  prot_insert_id.set_length_encoded_binary(true);

  is  >> prot_affected_rows
      >> prot_insert_id
      >> prot_server_status
      >> prot_warning_count;

  if (message_size > 0)
  {
    Protocol_chunk_string prot_message(ok.message, message_size);
    is >> prot_message;
    ok.message[message_size]= '\0';
  }
}

void prot_parse_eof_message(std::istream &is, struct st_eof_package &eof)
{
  Protocol_chunk<uint16_t> proto_warning_count(eof.warning_count);
  Protocol_chunk<uint16_t> proto_status_flags(eof.status_flags);

  is >> proto_warning_count
     >> proto_status_flags;
}

void proto_get_handshake_package(std::istream &is,
                                 struct st_handshake_package &p,
                                 int packet_length)
{
  uint8_t filler;
  uint8_t filler2[13];

  Protocol_chunk<uint8_t>   proto_protocol_version(p.protocol_version);
  Protocol_chunk<uint32_t>  proto_thread_id(p.thread_id);
  Protocol_chunk<uint8_t>   proto_scramble_buffer(p.scramble_buff, 8);
  Protocol_chunk<uint8_t>   proto_filler(filler);
  Protocol_chunk<uint16_t>  proto_server_capabilities(p.server_capabilities);
  Protocol_chunk<uint8_t>   proto_server_language(p.server_language);
  Protocol_chunk<uint16_t>  proto_server_status(p.server_status);
  Protocol_chunk<uint8_t>   proto_filler2(filler2,13);
  Protocol_chunk<uint8_t>   proto_scramble_buffer2(p.scramble_buff2, 13);

  is  >> proto_protocol_version
      >> p.server_version_str
      >> proto_thread_id
      >> proto_scramble_buffer
      >> proto_filler
      >> proto_server_capabilities
      >> proto_server_language
      >> proto_server_status
      >> proto_filler2
      >> proto_scramble_buffer2;

  //assert(filler == 0);

  int remaining_bytes= packet_length - 9+13+13+8;
  uint8_t extention_buffer[remaining_bytes];
  if (remaining_bytes > 0)
  {
    Protocol_chunk<uint8_t> proto_extension(extention_buffer, remaining_bytes);
    is >> proto_extension;
  }

}

void write_packet_header(char *buff, uint16_t size, uint8_t packet_no)
{
  int3store(buff, size);
  buff[3]= (char)packet_no;
}


buffer_source &operator>>(buffer_source &src, Protocol &chunk)
{
  char ch;
  int ct= 0;
  char *ptr= (char*)chunk.data();

  while(ct < chunk.size() && src.m_ptr < src.m_size)
  {
    ptr[ct]= src.m_src[src.m_ptr];
    ++ct;
    ++src.m_ptr;
  }
  return src;
}

std::istream &operator>>(std::istream &is, Protocol &chunk)
{
 if (chunk.is_length_encoded_binary())
  {
    int ct= 0;
    is.read((char *)chunk.data(),1);
    unsigned char byte= *(unsigned char *)chunk.data();
    if (byte < 250)
    {
      chunk.collapse_size(1);
      return is;
    }
    else if (byte == 251)
    {
      // is this a row data packet? if so, then this column value is NULL
      chunk.collapse_size(1);
      ct= 1;
    }
    else if (byte == 252)
    {
      chunk.collapse_size(2);
      ct= 1;
    }
    else if(byte == 253)
    {
      chunk.collapse_size(3);
      ct= 1;
    }

    /* Read remaining bytes */
    //is.read((char *)chunk.data(), chunk.size()-1);
    char ch;
    char *ptr= (char*)chunk.data();
    while(ct < chunk.size())
    {
      is.get(ch);
      ptr[ct]= ch;
      ++ct;
    }
  }
  else
  {
    char ch;
    int ct= 0;
    char *ptr= (char*)chunk.data();
    int sz= chunk.size();
    while(ct < sz)
    {
      is.get(ch);
      ptr[ct]= ch;
      ++ct;
    }
  }

  return is;
}

std::istream &operator>>(std::istream &is, std::string &str)
{
  std::ostringstream out;
  char ch;
  int ct= 0;
  do
  {
    is.get(ch);
    out.put(ch);
    ++ct;
  } while (is.good() && ch != '\0');
  str.append(out.str());
  return is;
}

std::istream &operator>>(std::istream &is, Protocol_chunk_string &str)
{
  char ch;
  int ct= 0;
  int sz= str.m_str->size();
  for (ct=0; ct< sz && is.good(); ct++)
  {
    is.get(ch);
    str.m_str->at(ct)= ch;
  }

  return is;
}

std::istream &operator>>(std::istream &is, Protocol_chunk_string_len &lenstr)
{
  uint8_t len;
  std::string *str= lenstr.m_storage;
  Protocol_chunk<uint8_t> proto_str_len(len);
  is >> proto_str_len;
  Protocol_chunk_string   proto_str(*str, len);
  is >> proto_str;
  return is;
}

std::ostream &operator<<(std::ostream &os, Protocol &chunk)
{
  if (!os.bad())
    os.write((const char *) chunk.data(),chunk.size());
  return os;
}

/* Removes trailing whitspaces from the input string */
void trim2(std::string& str)
{
  std::string::size_type pos = str.find_last_not_of('\0');
  if (pos != std::string::npos)
    str.erase(pos + 1);
  else
    str.clear();
}

Format_event *proto_format_desc_event(std::istream &is, Log_event_header *header)
{
  Format_event *fdev= new Format_event(header);
  int event_length= (fdev->header())->event_length;
  Protocol_chunk<uint16_t> proto_format_event_binlog_version(fdev->binlog_version);
  Protocol_chunk_string proto_format_event_master_version(fdev->master_version, 50);
  Protocol_chunk<uint32_t> proto_format_event_created_ts(fdev->created_ts);
  Protocol_chunk<uint8_t> proto_format_event_header_len(fdev->log_header_len);
  Protocol_chunk_vector proto_format_event_post_header(fdev->post_header_len,
                                                       event_length - 76);
  is >> proto_format_event_binlog_version
     >> proto_format_event_master_version
     >> proto_format_event_created_ts
     >> proto_format_event_header_len
     >> proto_format_event_post_header;

  /*
    Add an element at the beginning of the vector for UNKNOWN_EVENT
    in Log_event_type.
  */
  fdev->post_header_len.insert(fdev->post_header_len.begin(), 0);
  trim2(fdev->master_version);
  return fdev;

}
Query_event *proto_query_event(std::istream &is, Log_event_header *header)
{
  uint8_t db_name_len;
  uint16_t var_size;
  // Length of query stored in the payload.
  uint32_t query_len;
  Query_event *qev=new Query_event(header);
  Protocol_chunk<uint32_t> proto_query_event_thread_id(qev->thread_id);
  Protocol_chunk<uint32_t> proto_query_event_exec_time(qev->exec_time);
  Protocol_chunk<uint8_t> proto_query_event_db_name_len(db_name_len);
  Protocol_chunk<uint16_t> proto_query_event_error_code(qev->error_code);
  Protocol_chunk<uint16_t> proto_query_event_var_size(var_size);

  is >> proto_query_event_thread_id
     >> proto_query_event_exec_time
     >> proto_query_event_db_name_len
     >> proto_query_event_error_code
     >> proto_query_event_var_size;

  //TODO : Implement it in a better way.

  /*
    Query length =
    Total event length (header->event_length) -
      (
        (LOG_EVENT_HEADER_SIZE - 1) +  //Shouldn't LOG_EVENT_HEADER_SIZE=19?
        Thread-id (pre-defined, 4) +
        Execution time (pre-defined, 4) +
        Placeholder to store database length (pre-defined, 1) +
        Error code (pre-defined, 2) +
        Placeholder to store length taken by status variable blk
        (pre-defined, 2)+ Status variable block length (calculated, var_size) +
        Database name length (calculated, db_name_len) +
        Null terninator (pre-defined, 1) +
    )

    which gives :
  */

  query_len= header->event_length - (LOG_EVENT_HEADER_SIZE + 13 + var_size +
                                     db_name_len + 4);

  qev->variables.reserve(var_size);
  Protocol_chunk_vector proto_payload(qev->variables, var_size);
  is >> proto_payload;

  Protocol_chunk_string proto_query_event_db_name(qev->db_name,
                                                  (ulong)db_name_len);

  Protocol_chunk_string proto_query_event_query_str
    (qev->query, (ulong)query_len);

  char zero_marker;
  is >> proto_query_event_db_name
     >> zero_marker
     >> proto_query_event_query_str;
  // Following is not really required now,
  //qev->query.resize(qev->query.size() - 1);
  // Last character is a '\0' character.

  return qev;
}

Rotate_event *proto_rotate_event(std::istream &is, Log_event_header *header)
{
  Rotate_event *rev= new Rotate_event(header);

  uint32_t file_name_length= header->event_length - 7 - LOG_EVENT_HEADER_SIZE;

  Protocol_chunk<uint64_t > prot_position(rev->binlog_pos);
  Protocol_chunk_string prot_file_name(rev->binlog_file, file_name_length);
  is >> prot_position
     >> prot_file_name;

  return rev;
}

Incident_event *proto_incident_event(std::istream &is,
                                     Log_event_header *header)
{
  Incident_event *incident= new Incident_event(header);
  Protocol_chunk<uint8_t> proto_incident_code(incident->type);
  Protocol_chunk_string_len      proto_incident_message(incident->message);

  is >> proto_incident_code
     >> proto_incident_message;

  return incident;
}

Row_event *proto_rows_event(std::istream &is, Log_event_header *header)
{
  Row_event *rev=new Row_event(header);

  union
  {
    uint64_t integer;
    uint8_t bytes[6];
  } table_id;

  int bytes_read;
  table_id.integer= 0L;
  Protocol_chunk<uint8_t>  proto_table_id(&table_id.bytes[0], 6);
  Protocol_chunk<uint16_t> proto_flags(rev->flags);

  is >> proto_table_id
     >> proto_flags;

  bytes_read= proto_table_id.size() + proto_flags.size();

  if (header->type_code == WRITE_ROWS_EVENT ||
      header->type_code == DELETE_ROWS_EVENT ||
      header->type_code == UPDATE_ROWS_EVENT)
  {
    /*
      Have variable length header, check length,
      which includes length bytes
    */
    Protocol_chunk<uint16_t> proto_var_header_len(rev->var_header_len);
    is >> proto_var_header_len;

    if (rev->var_header_len < 2)
      return NULL;

    Protocol_chunk_vector proto_extra_header_data(rev->extra_header_data,
                                                  rev->var_header_len - 2);
    is >> proto_extra_header_data;

    bytes_read+= proto_var_header_len.size() + proto_extra_header_data.size();
  }

  Protocol_chunk<uint64_t> proto_column_len(rev->columns_len);
  proto_column_len.set_length_encoded_binary(true);

  is >> proto_column_len;

  rev->table_id=table_id.integer;
  int used_column_len=(int) ((rev->columns_len + 7) / 8);
  Protocol_chunk_vector proto_used_columns(rev->used_columns, used_column_len);
  rev->null_bits_len= used_column_len;

  is >> proto_used_columns;
  bytes_read+= proto_column_len.size() + used_column_len;

  if (header->type_code == UPDATE_ROWS_EVENT ||
      header->type_code == UPDATE_ROWS_EVENT_V1)
  {
    Protocol_chunk_vector proto_columns_before_image(rev->columns_before_image,
                                                     used_column_len);
    is >> proto_columns_before_image;
    bytes_read+=used_column_len;
  }

  ulong row_len= header->event_length - bytes_read - LOG_EVENT_HEADER_SIZE + 1;
  Protocol_chunk_vector proto_row(rev->row, row_len);
  is >> proto_row;

  return rev;
}

Int_var_event *proto_intvar_event(std::istream &is, Log_event_header *header)
{
  Int_var_event *event= new Int_var_event(header);

  Protocol_chunk<uint8_t>  proto_type(event->type);
  Protocol_chunk<uint64_t> proto_value(event->value);
  is >> proto_type
     >> proto_value;

  return event;
}

User_var_event *proto_uservar_event(std::istream &is, Log_event_header *header)
{
  User_var_event *event= new User_var_event(header);

  uint32_t name_len;
  Protocol_chunk<uint32_t> proto_name_len(name_len);

  is >> proto_name_len;

  Protocol_chunk_string proto_name(event->name, name_len);
  Protocol_chunk<uint8_t>  proto_null(event->is_null);

  is >> proto_name >> proto_null;
  if (event->is_null)
  {
    event->type = User_var_event::STRING_TYPE;
    event->charset = 63;                        // Binary charset
  }
  else
  {
    uint32_t value_len;
    Protocol_chunk<uint8_t> proto_type(event->type);
    Protocol_chunk<uint32_t> proto_charset(event->charset);
    Protocol_chunk<uint32_t> proto_val_len(value_len);
    is >> proto_type >> proto_charset >> proto_val_len;
    Protocol_chunk_string proto_value(event->value, value_len);
    is >> proto_value;
  }

  return event;
}

Table_map_event *proto_table_map_event(std::istream &is,
                                       Log_event_header *header)
{
  Table_map_event *tmev= new Table_map_event(header);
  uint64_t columns_len= 0;
  uint64_t metadata_len= 0;
  union
  {
    uint64_t integer;
    uint8_t bytes[6];
  } table_id;
  char zero_marker= 0;

  table_id.integer= 0L;
  Protocol_chunk<uint8_t> proto_table_id(&table_id.bytes[0], 6);
  Protocol_chunk<uint16_t> proto_flags(tmev->flags);
  Protocol_chunk_string_len proto_db_name(tmev->db_name);
  Protocol_chunk<uint8_t> proto_marker(zero_marker); // Should be '\0'
  Protocol_chunk_string_len proto_table_name(tmev->table_name);
  Protocol_chunk<uint64_t> proto_columns_len(columns_len);
  proto_columns_len.set_length_encoded_binary(true);

  is >> proto_table_id
     >> proto_flags
     >> proto_db_name
     >> proto_marker
     >> proto_table_name
     >> proto_marker
     >> proto_columns_len;
  tmev->table_id= table_id.integer;
  Protocol_chunk_vector proto_columns(tmev->columns, columns_len);
  Protocol_chunk<uint64_t> proto_metadata_len(metadata_len);
  proto_metadata_len.set_length_encoded_binary(true);

  is >> proto_columns
     >> proto_metadata_len;
  Protocol_chunk_vector proto_metadata(tmev->metadata, (ulong)metadata_len);
  is >> proto_metadata;
  ulong null_bits_len= (int)((tmev->columns.size() + 7) / 8);

  Protocol_chunk_vector proto_null_bits(tmev->null_bits, null_bits_len);

  is >> proto_null_bits;
  return tmev;
}

std::istream &operator>>(std::istream &is, Protocol_chunk_vector &chunk)
{
  ulong size= chunk.m_size;
  for(int i= 0; i < size; i++)
  {
    char ch;
    is.get(ch);
    chunk.m_vec->push_back(ch);
  }
  return is;
}


} } // end namespace mysql::system
