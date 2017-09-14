#include <stdlib.h>
#include <string.h>
#include "ipfs/util/errs.h"
#include "ipfs/util/time.h"
#include "ipfs/namesys/pb.h"
#include "ipfs/namesys/publisher.h"

/**
 * Convert an ipns_entry into a char array
 *
 * @param entry the ipns_entry
 * @returns a char array that contains the data from the ipns_entry
 */
char* ipns_entry_data_for_sig (struct ipns_entry *entry)
{
    char *ret;

    if (!entry || !entry->value || !entry->validity) {
        return NULL;
    }
    ret = calloc (1, strlen(entry->value) + strlen (entry->validity) + sizeof(IpnsEntry_ValidityType) + 1);
    if (ret) {
        strcpy(ret, entry->value);
        strcat(ret, entry->validity);
        if (entry->validityType) {
            memcpy(ret+strlen(entry->value)+strlen(entry->validity), entry->validityType, sizeof(IpnsEntry_ValidityType));
        } else {
            memcpy(ret+strlen(entry->value)+strlen(entry->validity), &IpnsEntry_EOL, sizeof(IpnsEntry_ValidityType));
        }
    }
    return ret;
}

int ipns_selector_func (int *idx, struct ipns_entry ***recs, char *k, char **vals)
{
    int err = 0, i, c;

    if (!idx || !recs || !k || !vals) {
        return ErrInvalidParam;
    }

    for (c = 0 ; vals[c] ; c++); // count array

    *recs = calloc(c+1, sizeof (void*)); // allocate return array.
    if (!*recs) {
        return ErrAllocFailed;
    }
    for (i = 0 ; i < c ; i++) {
        *recs[i] = calloc(1, sizeof (struct ipns_entry)); // alloc every record
        if (!*recs[i]) {
            return ErrAllocFailed;
        }
        //err = proto.Unmarshal(vals[i], *recs[i]); // and decode.
        if (err) {
            ipfs_namesys_ipnsentry_reset (*recs[i]); // make sure record is empty.
        }
    }
    return ipns_select_record(idx, *recs, vals);
}

/***
 * selects an ipns_entry record from a list
 *
 * @param idx the index of the found record
 * @param recs the records
 * @param vals the search criteria?
 * @returns 0 on success, otherwise error code
 */
int ipns_select_record (int *idx, struct ipns_entry **recs, char **vals)
{
    int err, i, best_i = -1, best_seq = 0;
    struct timespec rt, bestt;

    if (!idx || !recs || !vals) {
        return ErrInvalidParam;
    }

    for (i = 0 ; recs[i] ; i++) {
        if (!(recs[i]->sequence) || *(recs[i]->sequence) < best_seq) {
            continue;
        }

        if (best_i == -1 || *(recs[i]->sequence) > best_seq) {
            best_seq = *(recs[i]->sequence);
            best_i = i;
        } else if (*(recs[i]->sequence) == best_seq) {
            err = ipfs_util_time_parse_RFC3339 (&rt, ipfs_namesys_pb_get_validity (recs[i]));
            if (err) {
                continue;
            }
            err = ipfs_util_time_parse_RFC3339 (&bestt, ipfs_namesys_pb_get_validity (recs[best_i]));
            if (err) {
                continue;
            }
            if (rt.tv_sec > bestt.tv_sec || (rt.tv_sec == bestt.tv_sec && rt.tv_nsec > bestt.tv_nsec)) {
                best_i = i;
            } else if (rt.tv_sec == bestt.tv_sec && rt.tv_nsec == bestt.tv_nsec) {
                if (memcmp(vals[i], vals[best_i], strlen(vals[best_i])) > 0) { // FIXME: strlen?
                    best_i = i;
                }
            }
        }
    }
    if (best_i == -1) {
        return ErrNoRecord;
    }
    *idx = best_i;
    return 0;
}

/****
 * implements ValidatorFunc and verifies that the
 * given 'val' is an IpnsEntry and that that entry is valid.
 *
 * @param k
 * @param val
 * @returns 0 on success, otherwise an error code
 */
int ipns_validate_ipns_record (char *k, char *val)
{
    int err = 0;
    struct ipns_entry *entry = ipfs_namesys_pb_new_ipns_entry();
    struct timespec ts, now;

    if (!entry) {
        return ErrAllocFailed;
    }
    //err = proto.Unmarshal(val, entry);
    if (err) {
        return err;
    }
    if (ipfs_namesys_pb_get_validity_type (entry) == IpnsEntry_EOL) {
        err = ipfs_util_time_parse_RFC3339 (&ts, ipfs_namesys_pb_get_validity (entry));
        if (err) {
            //log.Debug("failed parsing time for ipns record EOL")
            return err;
        }
        timespec_get (&now, TIME_UTC);
        if (now.tv_nsec > ts.tv_nsec || (now.tv_nsec == ts.tv_nsec && now.tv_nsec > ts.tv_nsec)) {
            return ErrExpiredRecord;
        }
    } else {
        return ErrUnrecognizedValidity;
    }
   return 0;
}

/**
 * Helper to copy values from one to another, allocating memory
 *
 * @param from the value to copy
 * @param from_size the size of from
 * @param to where to allocate memory and copy
 * @param to_size where to put the value of from_size in the new structure
 * @returns true(1) on success, false(0) otherwise
 */
int ipfs_namesys_copy_bytes(uint8_t* from, int from_size, uint8_t** to, int* to_size) {
	*to = (uint8_t*) malloc(from_size);
	if (*to == NULL) {
		return 0;
	}
	memcpy(*to, from, from_size);
	*to_size = from_size;
	return 1;
}

/**
 * Store the hash locally, and notify the network
 *
 * @param local_node the context
 * @param cid the hash
 * @returns true(1) on success, false(0) otherwise
 */
int ipfs_namesys_publish(struct IpfsNode* local_node, struct Cid* cid) {
	// store locally
	struct DatastoreRecord* record = libp2p_datastore_record_new();
	if (record == NULL)
		return 0;

	// key
	if (!ipfs_namesys_copy_bytes(cid->hash, cid->hash_length, &record->key, &record->key_size)) {
		libp2p_datastore_record_free(record);
		return 0;
	}
	// value
	if (!ipfs_namesys_copy_bytes(local_node->identity->peer->id, local_node->identity->peer->id_size, &record->value, &record->value_size)) {
		libp2p_datastore_record_free(record);
		return 0;
	}

	if (!local_node->repo->config->datastore->datastore_put(record, local_node->repo->config->datastore)) {
		libp2p_datastore_record_free(record);
		return 0;
	}
	libp2p_datastore_record_free(record);

	// propagate to network
	// build the KademliaMessage
	struct KademliaMessage* msg = libp2p_message_new();
	if (msg == NULL) {
		libp2p_message_free(msg);
		return 0;
	}
	msg->provider_peer_head = libp2p_utils_vector_new(1);
	libp2p_utils_vector_add(msg->provider_peer_head, local_node->identity->peer);
	// msg->Libp2pRecord
	msg->record = libp2p_record_new();
	if (msg->record == NULL) {
		libp2p_message_free(msg);
		return 0;
	}
	// KademliaMessage->Libp2pRecord->author
	if (!ipfs_namesys_copy_bytes(local_node->identity->peer->id, local_node->identity->peer->id_size, &msg->record->author, &msg->record->author_size)) {
		libp2p_message_free(msg);
		return 0;
	}
	// KademliaMessage->Libp2pRecord->key
	if (!ipfs_namesys_copy_bytes(cid->hash, cid->hash_length, &msg->record->key, &msg->record->key_size)) {
		libp2p_message_free(msg);
		return 0;
	}
	// KademliaMessage->Libp2pRecord->value
	if (!ipfs_namesys_copy_bytes(local_node->identity->peer->id, local_node->identity->peer->id_size, &msg->record->value, &msg->record->value_size)) {
		libp2p_message_free(msg);
		return 0;
	}

	int retVal = libp2p_routing_send_message(local_node->identity->peer, local_node->providerstore, msg);
	libp2p_message_free(msg);
	return retVal;
}
