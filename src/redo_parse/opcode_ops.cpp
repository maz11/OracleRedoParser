#include <iostream>
#include <string.h>
#include <set>
#include <utility>

#include "opcode_ops.h"
#include "opcode.h"
#include "util/dtypes.h"
#include "util/logger.h"

namespace databus {
  bool validOp(Ushort op) {
    const std::set<Ushort> valid_ops{
        opcode::kOther,       opcode::kUndo,        opcode::kBeginTrans,
        opcode::kCommit,      opcode::kNewSession,  opcode::kSwitchSession,
        opcode::kInsert,      opcode::kDelete,      opcode::kUpdate,
        opcode::kRowChain,    opcode::kMfc,         opcode::kCfa,
        opcode::kMultiInsert, opcode::kMultiDelete, opcode::kLmn,
        opcode::kDdl};
    return valid_ops.find(op) != valid_ops.end();
  }

  XID Ops0501::getXID(const ChangeHeader* change0501) {
    OpCode0501* op51 = (OpCode0501*)change0501->part(1);
    XID xid_high = op51->xid_high_;
    XID xid_mid = op51->xid_mid_;
    XID xid_low = op51->xid_low_;
    return xid_high << ((sizeof(uint32_t) + sizeof(Ushort)) * 8) |
           xid_mid << (sizeof(uint32_t) * 8) | xid_low;
  }

  uint32_t Ops0501::getObjId(const ChangeHeader* change0501) {
    OpCode0501Sec* op51sec = (OpCode0501Sec*)change0501->part(2);
    return op51sec->object_id_;
  }

  uint32_t Ops0501::getDataObjId(const ChangeHeader* change0501) {
    OpCode0501Sec* op51sec = (OpCode0501Sec*)change0501->part(2);
    return op51sec->data_object_id_;
  }

  Row _makeUpUncommCols(const char* start, int total_cols) {
    Row changes;
    for (int i = 0; i < total_cols; ++i) {
      start++;
      Ushort len = *((Ushort*)start);
      start += sizeof(Ushort);
      Ushort col_id = i + 1;
      char* content = new char[len + 1];
      memcpy(content, start, len);
      content[len] = '\0';
      start += len;
      ColumnChange* col_change = new ColumnChange(col_id, len, content);
      changes.push_back(col_change);
    }
    return changes;
  }

  Row _makeUpLenPrefixCols(Ushort* col_num, Ushort total_cols,
                           const ChangeHeader* change, Ushort data_offset) {
    // std::cout << "Makeup Len Prefix columns " << std::endl;
    Row row;
    for (int i = 0; i < total_cols; ++i) {
      const char* src = change->part(data_offset + i);
      Ushort len = *((Ushort*)src);
      char* data = new char[len + 1];
      memcpy(data, src + sizeof(Ushort), len);
      data[len] = '\0';
      ColumnChange* col_change = new ColumnChange(*(col_num + i), len, data);
      row.push_back(col_change);
    }
    return row;
  }

  Row _makeUpNoLenPrefixCols(Ushort* col_num, Ushort total_cols,
                             const ChangeHeader* change, Ushort data_offset,
                             bool supplemental) {
    // std::cout << "Makeup no len in data part columns " << std::endl;
    Row row;
    Ushort* col_len;
    if (supplemental)
      col_len = (Ushort*)change->part(data_offset - 1);
    else
      col_len = change->partLen(data_offset);
    for (Ushort i = 0; i < total_cols; ++i) {
      const char* src = change->part(data_offset + i);
      char* data = new char[*(col_len + i) + 1];
      memcpy(data, src, *(col_len + i));
      data[*(col_len + i)] = '\0';
      ColumnChange* col_change = NULL;
      if (supplemental)
        col_change = new ColumnChange(*(col_num + i) - 1, *(col_len + i), data);
      else if (col_num == NULL)
        col_change = new ColumnChange(i, *(col_len + i), data);
      else
        col_change = new ColumnChange(*(col_num + i), *(col_len + i), data);
      row.push_back(col_change);
    }
    return row;
  }

  Row makeUpCols(Ushort* col_num, Ushort total_cols, const ChangeHeader* change,
                 Ushort data_offset, Ushort xtype, bool supplemental) {
    if (xtype & 0x80)
      return _makeUpLenPrefixCols(col_num, total_cols, change, data_offset);
    else
      return _makeUpNoLenPrefixCols(col_num, total_cols, change, data_offset,
                                    supplemental);
  }

  void Ops0501::printTransBase(const ChangeHeader* change0501) {
    std::cout << std::endl << "xid: " << getXID(change0501)
              << " object_id: " << getObjId(change0501)
              << " data_object_id: " << getDataObjId(change0501) << std::endl;
  }

  std::list<Row> Ops0501::makeUpUndo(const ChangeHeader* change0501) {
    std::list<Row> rows;
    Row row;
    OpCodeKdo* opkdo = (OpCodeKdo*)change0501->part(4);
    OpCode0501Sec* sec = (OpCode0501Sec*)(change0501->part(2));
    // printTransBase(change0501);
    // if there any exception if opkdo->opcode_ = 0501
    switch (opkdo->opcode_ & 0x1f) {
      case opcode::kInsert & 0xff: {
        // common delete will go to here
        OpCodeKdoirp* irp = (OpCodeKdoirp*)opkdo;
        if (irp->column_count_ != 0) {
          Row undo_cols = makeUpCols((Ushort*)NULL, irp->column_count_,
                                     change0501, 5, irp->xtype_, false);
          row.splice(row.end(), undo_cols);
        }
        Ushort part_no;
        if (sec->op_major_ == 0x05 && sec->op_minor_ == 0x01) {
          part_no = 7 + irp->column_count_;
        } else {
          part_no = 6 + irp->column_count_;
        }
        if (irp->xtype_ & 0x20) {
          OpCodeSupplemental* opsup =
              (OpCodeSupplemental*)change0501->part(part_no++);
          Row suplemental_cols = _makeUpNoLenPrefixCols(
              (Ushort*)change0501->part(part_no), opsup->total_cols_,
              change0501, part_no + 2, true);

          if (!suplemental_cols.empty()) {
            row.splice(row.end(), suplemental_cols);
          }
        }
      } break;
      case opcode::kMultiInsert & 0xff: {
        BOOST_LOG_TRIVIAL(fatal) << "seems run into op mulit-delete, exiting"
                                 << std::endl;
        std::exit(100);
      } break;

      case opcode::kRowChain & 0xff:
        return rows;
      case opcode::kUpdate & 0xff: {
        OpCodeKdourp* urp = (OpCodeKdourp*)change0501->part(4);
        int total_colums = urp->ncol_;
        Ushort total_changes = urp->nchanged_;
        row = makeUpCols((Ushort*)change0501->part(5), total_changes,
                         change0501, 6, urp->xtype_);

        Ushort part_num = 6 + total_changes;

        if (urp->opcode_ & 0x40) ++part_num;
        if (urp->opcode_ & 0x20) {
          OpCodeSupplemental* suplemental_op_header =
              (OpCodeSupplemental*)change0501->part(part_num++);
          Row suplemental_cols =
              _makeUpNoLenPrefixCols((Ushort*)change0501->part(part_num),
                                     suplemental_op_header->total_cols_,
                                     change0501, part_num + 2, true);
          row.splice(row.end(), suplemental_cols);
        }
      } break;
      case opcode::kDelete & 0xff: {
        // Seems this code will be never used, not for simple delete at least
        OpCodeKdodrp* drp = (OpCodeKdodrp*)change0501->part(4);
        if (drp->opcode_ & 0x20) {
          OpCodeSupplemental* sup = (OpCodeSupplemental*)change0501->part(5);
          if (sup->total_cols_ > 0) {
            row = _makeUpNoLenPrefixCols((Ushort*)change0501->part(6),
                                         sup->total_cols_, change0501, 8, true);
          }
        }
      } break;
      case opcode::kMultiDelete & 0xff:
        return rows;
      case opcode::kLmn & 0xff:
        return rows;
      case opcode::kMfc & 0xff:
        return rows;
      case opcode::kCfa & 0xff:
        return rows;
      default:
        return rows;
    }

    rows.push_back(std::move(row));
    return rows;
  }

  std::list<Row> OpsDML::makeUpRedoCols(const ChangeHeader* change) {
    OpCodeKdo* kdo = (OpCodeKdo*)change->part(2);
    std::list<Row> redo_rows;
    Row redo_row;
    switch (change->opCode()) {
      case opcode::kInsert:
        redo_row = makeUpCols(NULL, ((OpCodeKdoirp*)kdo)->column_count_, change,
                              3, ((OpCodeKdoirp*)kdo)->xtype_, false);
        break;
      case opcode::kUpdate:
        redo_row = makeUpCols((Ushort*)change->part(3),
                              ((OpCodeKdourp*)kdo)->nchanged_, change, 4,
                              kdo->xtype_, false);
        break;
      case opcode::kMultiInsert:
        break;
      default:
        break;
    }
    redo_rows.push_back(std::move(redo_row));
    return redo_rows;
  }
}
