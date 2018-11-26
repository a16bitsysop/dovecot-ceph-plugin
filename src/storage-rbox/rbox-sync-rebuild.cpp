
// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Copyright (c) 2017-2018 Tallence AG and the authors
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */
#include <list>
extern "C" {
#include "dovecot-all.h"

#include "rbox-sync.h"
#include "debug-helper.h"
#include "data-stack.h"
}

#include "rbox-sync-rebuild.h"

#include "rbox-storage.hpp"
#include "rbox-mail.h"
#include "encoding.h"
#include "../librmb/rados-mail.h"
#include "rados-util.h"

using librmb::RadosMail;
using librmb::rbox_metadata_key;

int rbox_sync_add_object(struct index_rebuild_context *ctx, const std::string &oi, librmb::RadosMail *mail_obj,
                         bool alt_storage, uint32_t next_uid) {
  FUNC_START();
  struct rbox_mailbox *rbox = (struct rbox_mailbox *)ctx->box;
  std::string xattr_mail_uid;
  mail_obj->get_metadata(rbox_metadata_key::RBOX_METADATA_MAIL_UID, &xattr_mail_uid);
  std::string xattr_guid;
  mail_obj->get_metadata(rbox_metadata_key::RBOX_METADATA_GUID, &xattr_guid);
  struct mail_storage *storage = ctx->box->storage;
  struct rbox_storage *r_storage = (struct rbox_storage *)storage;
  uint32_t seq;

  mail_index_append(ctx->trans, next_uid, &seq);
#ifdef DEBUG
  i_debug("added to index %d", next_uid);
#endif
  /* save the 128bit GUID/OID to index record */
  struct obox_mail_index_record rec;
  i_zero(&rec);
  // convert oid and guid to
  guid_128_t oid;
  if (guid_128_from_string(oi.c_str(), oid) < 0) {
    i_error("guid_128 oi.c_str() string (%s), next_uid(%d)", oi.c_str(), next_uid);
    FUNC_END();
    return -1;
  }
  guid_128_t guid;
  if (guid_128_from_string(xattr_guid.c_str(), guid) < 0) {
    i_error("guid_128 xattr_guid string '%s', next_uid(%d)", xattr_guid.c_str(), next_uid);
    FUNC_END();
    return -1;
  }
  memcpy(rec.guid, guid, sizeof(guid));
  memcpy(rec.oid, oid, sizeof(oid));

  mail_index_update_ext(ctx->trans, seq, rbox->ext_id, &rec, NULL);
  if (alt_storage) {
    mail_index_update_flags(ctx->trans, seq, MODIFY_ADD, (enum mail_flags)RBOX_INDEX_FLAG_ALT);
  }

  T_BEGIN { index_rebuild_index_metadata(ctx, seq, next_uid); }
  T_END;

  // update uid.
  librmb::RadosMetadata mail_uid(librmb::RBOX_METADATA_MAIL_UID, next_uid);
  std::string s_oid = mail_obj->get_oid();
  std::list<librmb::RadosMetadata> to_update;
  to_update.push_back(mail_uid);
  if (!r_storage->ms->get_storage()->update_metadata(s_oid, to_update)) {
    i_warning("update of MAIL_UID failed: for object: %s , uid: %d", mail_obj->get_oid().c_str(), next_uid);
  }
#ifdef DEBUG
  i_debug("rebuilding %s , with oid=%d", oi.c_str(), next_uid);
#endif
  FUNC_END();
  return 0;
}
// find objects with mailbox_guid 'U' attribute
int rbox_sync_rebuild_entry(struct index_rebuild_context *ctx, librados::NObjectIterator &iter,
                            struct rbox_sync_rebuild_ctx *rebuild_ctx) {
  FUNC_START();
  struct mail_storage *storage = ctx->box->storage;
  struct rbox_storage *r_storage = (struct rbox_storage *)storage;

  // find all objects with x attr M = mailbox_guid
  // if non is found : set mailbox_deleted and mail_storage_set_critical...

  const struct mail_index_header *hdr = mail_index_get_header(ctx->trans->view);

  if (rebuild_ctx->next_uid == INT_MAX) {
    rebuild_ctx->next_uid = hdr->next_uid != 0 ? hdr->next_uid : 1;
  }

  int found = 0;
  int sync_add_objects_ret = 0;
  while (iter != librados::NObjectIterator::__EndObjectIterator) {
    std::map<std::string, ceph::bufferlist> attrset;
    librmb::RadosMail mail_object;
    mail_object.set_oid((*iter).get_oid());
    int load_metadata_ret;
    if (rebuild_ctx->alt_storage) {
      r_storage->ms->get_storage()->set_io_ctx(&r_storage->alt->get_io_ctx());
      load_metadata_ret = r_storage->ms->get_storage()->load_metadata(&mail_object);
    } else {
      load_metadata_ret = r_storage->ms->get_storage()->load_metadata(&mail_object);
    }

    if (!librmb::RadosUtils::validate_metadata(mail_object.get_metadata())) {
      i_error("metadata for object : %s is not valid, skipping object ", mail_object.get_oid().c_str());
      ++iter;
      continue;
    }
    if (load_metadata_ret >= 0) {
      sync_add_objects_ret =
          rbox_sync_add_object(ctx, (*iter).get_oid(), &mail_object, rebuild_ctx->alt_storage, rebuild_ctx->next_uid);
      if (sync_add_objects_ret < 0) {
        i_error("sync_add_object: oid(%s), alt_storage(%d),uid(%d)", (*iter).get_oid().c_str(),
                rebuild_ctx->alt_storage, rebuild_ctx->next_uid);
        break;
      }
    }
    ++iter;
    ++found;
    ++rebuild_ctx->next_uid;
  }
  if (sync_add_objects_ret < 0) {
    i_error("error rbox_sync_add_objects for mbox %s", ctx->box->name);
    mailbox_set_deleted(ctx->box);
    mail_storage_set_critical(storage, "find mailbox(%s) failed: %m", ctx->box->name);
    FUNC_END();
    return -1;
  }

  if (found == 0) {
#ifdef DEBUG
    i_debug("no entry to restore can be found for mailbox %s", ctx->box->name);
#endif
    mailbox_set_deleted(ctx->box);
    FUNC_END();
    return 0;
  }

  FUNC_END();
  return sync_add_objects_ret;
}

void rbox_sync_set_uidvalidity(struct index_rebuild_context *ctx) {
  FUNC_START();
  uint32_t uid_validity;

  /* if uidvalidity is set in the old index, use it */
  uid_validity = mail_index_get_header(ctx->view)->uid_validity;
  if (uid_validity == 0)
    uid_validity = rbox_get_uidvalidity_next(ctx->box->list);

  mail_index_update_header(ctx->trans, offsetof(struct mail_index_header, uid_validity), &uid_validity,
                           sizeof(uid_validity), TRUE);
  FUNC_END();
}

int search_objects(struct index_rebuild_context *ctx, struct rbox_sync_rebuild_ctx *rebuild_ctx) {
  FUNC_START();
  struct rbox_mailbox *rbox = (struct rbox_mailbox *)ctx->box;
  struct rbox_storage *r_storage = (struct rbox_storage *)ctx->box->storage;
  librmb::RadosStorage *storage = rebuild_ctx->alt_storage ? r_storage->alt : r_storage->s;
  int ret = 0;
  std::string guid(guid_128_to_string(rbox->mailbox_guid));
  librmb::RadosMetadata attr_guid(rbox_metadata_key::RBOX_METADATA_MAILBOX_GUID, guid);
  // rebuild index.

  librados::NObjectIterator iter_guid(storage->find_mails(&attr_guid));
  ret = rbox_sync_rebuild_entry(ctx, iter_guid, rebuild_ctx);
  FUNC_END();
  return ret;
}
void rbox_sync_update_header(struct index_rebuild_context *ctx) {
  FUNC_START();
  struct rbox_mailbox *rbox = (struct rbox_mailbox *)ctx->box;
  struct rbox_index_header hdr;
  bool need_resize;

  if (rbox_read_header(rbox, &hdr, FALSE, &need_resize) < 0)
    i_zero(&hdr);
  if (guid_128_is_empty(hdr.mailbox_guid))
    guid_128_generate(hdr.mailbox_guid);
  if (++hdr.rebuild_count == 0)
    hdr.rebuild_count = 1;
  /* mailbox is being reset. this gets written directly there */
  mail_index_set_ext_init_data(ctx->box->index, rbox->hdr_ext_id, &hdr, sizeof(hdr));
  FUNC_END();
}
int rbox_sync_index_rebuild_objects(struct index_rebuild_context *ctx) {
  FUNC_START();
  int ret = 0;
  pool_t pool;
  rbox_sync_set_uidvalidity(ctx);
  struct rbox_sync_rebuild_ctx *rebuild_ctx;
  bool alt_storage = is_alternate_pool_valid(ctx->box);

  if (rbox_open_rados_connection(ctx->box, alt_storage) < 0) {
    i_error("rbox_sync_index_rebuild_objects: cannot open rados connection");
    FUNC_END();
    return -1;
  }
  pool = pool_alloconly_create("rbox rebuild pool", 256);

  rebuild_ctx = p_new(pool, struct rbox_sync_rebuild_ctx, 1);
  i_zero(rebuild_ctx);
  rebuild_ctx->alt_storage = false;
  rebuild_ctx->next_uid = INT_MAX;

  search_objects(ctx, rebuild_ctx);
  if (alt_storage) {
    rebuild_ctx->alt_storage = true;
#ifdef DEBUG
    struct rbox_mailbox *rbox = (struct rbox_mailbox *)ctx->box;
    i_debug("ALT_STORAGE ACTIVE: '%s' ", rbox->box.list->set.alt_dir);
#endif
    search_objects(ctx, rebuild_ctx);
  }

  rbox_sync_update_header(ctx);
  pool_unref(&pool);
  FUNC_END();
  return ret;
}

int rbox_storage_rebuild_in_context(struct rbox_storage *r_storage, bool force) {
  FUNC_START();

  struct mail_user *user = r_storage->storage.user;

  struct mail_namespace *ns = mail_namespace_find_inbox(user->namespaces);
  for (; ns != NULL; ns = ns->next) {
    repair_namespace(ns, force);
  }

  FUNC_END();
  return 0;
}

int repair_namespace(struct mail_namespace *ns, bool force) {
  FUNC_START();
  struct mailbox_list_iterate_context *iter;
  const struct mailbox_info *info;
  int ret = 0;

  iter = mailbox_list_iter_init(
      ns->list, "*",
      static_cast<mailbox_list_iter_flags>(MAILBOX_LIST_ITER_RAW_LIST | MAILBOX_LIST_ITER_RETURN_NO_FLAGS));
  while ((info = mailbox_list_iter_next(iter)) != NULL) {
    if ((info->flags & (MAILBOX_NONEXISTENT | MAILBOX_NOSELECT)) == 0) {
      struct mailbox *box = mailbox_alloc(ns->list, info->vname, MAILBOX_FLAG_SAVEONLY);

      if (mailbox_open(box) < 0) {
        FUNC_END();
        return -1;
      }
      struct rbox_mailbox *rbox = (struct rbox_mailbox *)box;
      ret = rbox_sync_index_rebuild(rbox, force);
      if (ret < 0) {
        i_error("error resync (%s), error(%d), force(%d)", info->vname, ret, force);
      }
      mailbox_free(&box);
    }
  }
  if (mailbox_list_iter_deinit(&iter) < 0) {
    ret = -1;
  }

  FUNC_END();
  return ret;
}

int rbox_sync_index_rebuild(struct rbox_mailbox *rbox, bool force) {
  struct index_rebuild_context *ctx;
  struct mail_index_view *view;
  struct mail_index_transaction *trans;
  struct rbox_index_header hdr;
  bool need_resize;
  int ret;
  FUNC_START();
  // get mailbox guid
  if (!force && rbox_read_header(rbox, &hdr, FALSE, &need_resize) == 0) {
    if (hdr.rebuild_count != rbox->storage->corrupted_rebuild_count && hdr.rebuild_count != 0) {
      /* already rebuilt by someone else */
      i_warning("index already rebuild by someone else %d c_rebuild_count =%d", hdr.rebuild_count,
                rbox->storage->corrupted_rebuild_count);
      rbox->storage->corrupted_rebuild_count = 0;
      FUNC_END();
      return 0;
    }
#ifdef DEBUG
    i_debug("index could not be opened");
#endif
    // try to determine mailbox guid via xattr.
  }
  i_warning("rbox %s: Rebuilding index, guid: %s , mailbox_name: %s, alt_storage: %s", mailbox_get_path(&rbox->box),
            guid_128_to_string(rbox->mailbox_guid), rbox->box.name, rbox->box.list->set.alt_dir);

  view = mail_index_view_open(rbox->box.index);

  trans = mail_index_transaction_begin(view, MAIL_INDEX_TRANSACTION_FLAG_EXTERNAL);

  ctx = index_index_rebuild_init(&rbox->box, view, trans);

  ret = rbox_sync_index_rebuild_objects(ctx);

#ifdef DEBUG
  i_debug("rebuild finished");
#endif
  index_index_rebuild_deinit(&ctx, rbox_get_uidvalidity_next);

  if (ret < 0) {
    mail_index_transaction_rollback(&trans);
  } else {
#ifdef DOVECOT_CEPH_PLUGINS_HAVE_MAIL_INDEX_HDR_FLAG_FSCKD
    mail_index_unset_fscked(trans);
#endif
    ret = mail_index_transaction_commit(&trans);
  }
  hdr.rebuild_count++;
  rbox->storage->corrupted_rebuild_count = 0;
  mail_index_view_close(&view);
  FUNC_END();
  return ret;
}
