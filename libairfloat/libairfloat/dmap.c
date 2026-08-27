//
//  dmap.c
//  AirFloat
//
//  Copyright (c) 2013, Kristian Trenskow All rights reserved.
//
//  Redistribution and use in source and binary forms, with or
//  without modification, are permitted provided that the following
//  conditions are met:
//
//  Redistributions of source code must retain the above copyright
//  notice, this list of conditions and the following disclaimer.
//  Redistributions in binary form must reproduce the above
//  copyright notice, this list of conditions and the following
//  disclaimer in the documentation and/or other materials provided
//  with the distribution. THIS SOFTWARE IS PROVIDED BY THE
//  COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
//  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
//  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "Endian.h"
#include "log.h"

#include "DMAP.h"

#define DMAP_MAX_NESTING_DEPTH 32

struct dmap_atom_type {
    const uint32_t tag;
    const char* const identifier;
    const dmap_type type;
};

struct dmap_atom {
    uint32_t tag;
    uint32_t type;
    void* buffer;
    size_t size;
    struct dmap_t* container;
};

struct dmap_t {
    struct dmap_atom* atoms;
    uint32_t count;
};

const struct dmap_atom_type _atom_types[] = {
    { 'miid',     "dmap.itemid",                           dmap_type_long      },
    { 'minm',     "dmap.itemname",                         dmap_type_string    },
    { 'mikd',     "dmap.itemkind",                         dmap_type_longlong  },
    { 'mper',     "dmap.persistentid",                     dmap_type_unknown   },
    { 'mcon',     "dmap.container",                        dmap_type_container },
    { 'mcti',     "dmap.containeritemid",                  dmap_type_long      },
    { 'mpco',     "dmap.parentcontainerid",                dmap_type_long      },
    { 'mstt',     "dmap.status",                           dmap_type_long      },
    { 'msts',     "dmap.statusstring",                     dmap_type_string    },
    { 'mimc',     "dmap.itemcount",                        dmap_type_long      },
    { 'mctc',     "dmap.containercount",                   dmap_type_long      },
    { 'mrco',     "dmap.returnedcount",                    dmap_type_long      },
    { 'mtco',     "dmap.specifiedtotalcount",              dmap_type_long      },
    { 'f?ch',     "dmap.haschildcontainers",               dmap_type_char      },
    { 'mlcl',     "dmap.listing",                          dmap_type_container },
    { 'mlit',     "dmap.listingitem",                      dmap_type_container },
    { 'mbcl',     "dmap.bag",                              dmap_type_container },
    { 'mdcl',     "dmap.dictionary",                       dmap_type_container },
    { 'msrv',     "dmap.serverinforesponse",               dmap_type_container },
    { 'msau',     "dmap.authenticationmethod",             dmap_type_char      },
    { 'msas',     "dmap.authenticationschemes",            dmap_type_long      },
    { 'mslr',     "dmap.loginrequired",                    dmap_type_char      },
    { 'mpro',     "dmap.protocolversion",                  dmap_type_version   },
    { 'msal',     "dmap.supportsautologout",               dmap_type_char      },
    { 'msup',     "dmap.supportsupdate",                   dmap_type_char      },
    { 'mspi',     "dmap.supportspersistentids",            dmap_type_char      },
    { 'msex',     "dmap.supportsextensions",               dmap_type_char      },
    { 'msbr',     "dmap.supportsbrowse",                   dmap_type_char      },
    { 'msqy',     "dmap.supportsquery",                    dmap_type_char      },
    { 'msix',     "dmap.supportsindex",                    dmap_type_char      },
    { 'msrs',     "dmap.supportsresolve",                  dmap_type_char      },
    { 'mstm',     "dmap.timeoutinterval",                  dmap_type_long      },
    { 'msdc',     "dmap.databasescount",                   dmap_type_long      },
    { 'mstc',     "dmap.utctime",                          dmap_type_date      },
    { 'mlog',     "dmap.loginresponse",                    dmap_type_container },
    { 'mlid',     "dmap.sessionid",                        dmap_type_long      },
    { 'mupd',     "dmap.updateresponse",                   dmap_type_container },
    { 'musr',     "dmap.serverrevision",                   dmap_type_long      },
    { 'muty',     "dmap.updatetype",                       dmap_type_char      },
    { 'mudl',     "dmap.deletedid",                        dmap_type_container },
    { 'msdc',     "dmap.databasescount",                   dmap_type_long      },
    { 'mccr',     "dmap.contentcodesresponse",             dmap_type_container },
    { 'mcnm',     "dmap.contentcodesnumber",               dmap_type_long      },
    { 'mcna',     "dmap.contentcodesname",                 dmap_type_string    },
    { 'mcty',     "dmap.contentcodestype",                 dmap_type_short     },
    { 'meds',     "dmap.editcommandssupported",            dmap_type_long      },
    { 'ated',     "daap.supportsextradata",                dmap_type_short     },
    { 'apro',     "daap.protocolversion",                  dmap_type_version   },
    { 'avdb',     "daap.serverdatabases",                  dmap_type_container },
    { 'abro',     "daap.databasebrowse",                   dmap_type_container },
    { 'adbs',     "daap.databasesongs",                    dmap_type_container },
    { 'aply',     "daap.databaseplaylists",                dmap_type_container },
    { 'apso',     "daap.playlistsongs",                    dmap_type_container },
    { 'arsv',     "daap.resolve",                          dmap_type_container },
    { 'arif',     "daap.resolveinfo",                      dmap_type_container },
    { 'abal',     "daap.browsealbumlisting",               dmap_type_container },
    { 'abar',     "daap.browseartistlisting",              dmap_type_container },
    { 'abcp',     "daap.browsecomposerlisting",            dmap_type_container },
    { 'abgn',     "daap.browsegenrelisting",               dmap_type_container },
    { 'aePP',     "com.apple.itunes.is-podcast-playlist",  dmap_type_char      },
    { 'asal',     "daap.songalbum",                        dmap_type_string    },
    { 'asar',     "daap.songartist",                       dmap_type_string    },
    { 'asbr',     "daap.songbitrate",                      dmap_type_short     },
    { 'ascm',     "daap.songcomment",                      dmap_type_string    },
    { 'asco',     "daap.songcompilation",                  dmap_type_char      },
    { 'ascp',     "daap.songcomposer",                     dmap_type_string    },
    { 'asda',     "daap.songdateadded",                    dmap_type_date      },
    { 'asdm',     "daap.songdatemodified",                 dmap_type_date      },
    { 'asdc',     "daap.songdisccount",                    dmap_type_short     },
    { 'asdn',     "daap.songdiscnumber",                   dmap_type_short     },
    { 'aseq',     "daap.songeqpreset",                     dmap_type_string    },
    { 'asgn',     "daap.songgenre",                        dmap_type_string    },
    { 'asdt',     "daap.songdescription",                  dmap_type_string    },
    { 'assr',     "daap.songsamplerate",                   dmap_type_long      },
    { 'assz',     "daap.songsize",                         dmap_type_long      },
    { 'asst',     "daap.songstarttime",                    dmap_type_long      },
    { 'assp',     "daap.songstoptime",                     dmap_type_long      },
    { 'astm',     "daap.songtime",                         dmap_type_long      },
    { 'astc',     "daap.songtrackcount",                   dmap_type_short     },
    { 'astn',     "daap.songtracknumber",                  dmap_type_short     },
    { 'asur',     "daap.songuserrating",                   dmap_type_char      },
    { 'asyr',     "daap.songyear",                         dmap_type_short     },
    { 'asfm',     "daap.songformat",                       dmap_type_string    },
    { 'asdb',     "daap.songdisabled",                     dmap_type_char      },
    { 'asdk',     "daap.songdatakind",                     dmap_type_char      },
    { 'asul',     "daap.songdataurl",                      dmap_type_string    },
    { 'asbt',     "daap.songbeatsperminute",               dmap_type_short     },
    { 'abpl',     "daap.baseplaylist",                     dmap_type_char      },
    { 'agrp',     "daap.songgrouping",                     dmap_type_string    },
    { 'ascd',     "daap.songcodectype",                    dmap_type_long      },
    { 'ascs',     "daap.songcodecsubtype",                 dmap_type_long      },
    { 'apsm',     "daap.playlistshufflemode",              dmap_type_char      },
    { 'aprm',     "daap.playlistrepeatmode",               dmap_type_char      },
    { 'asct',     "daap.songcategory",                     dmap_type_string    },
    { 'ascn',     "daap.songcontentdescription",           dmap_type_string    },
    { 'aslc',     "daap.songlongcontentdescription",       dmap_type_string    },
    { 'asky',     "daap.songkeywords",                     dmap_type_string    },
    { 'ascr',     "daap.songcontentrating",                dmap_type_char      },
    { 'asgp',     "daap.songgapless",                      dmap_type_char      },
    { 'asdr',     "daap.songdatereleased",                 dmap_type_date      },
    { 'asdp',     "daap.songdatepurchased",                dmap_type_date      },
    { 'ashp',     "daap.songhasbeenplayed",                dmap_type_char      },
    { 'assn',     "daap.sortname",                         dmap_type_string    },
    { 'assa',     "daap.sortartist",                       dmap_type_string    },
    { 'assl',     "daap.sortalbumartist",                  dmap_type_string    },
    { 'assu',     "daap.sortalbum",                        dmap_type_string    },
    { 'assc',     "daap.sortcomposer",                     dmap_type_string    },
    { 'asss',     "daap.sortseriesname",                   dmap_type_string    },
    { 'asbk',     "daap.bookmarkable",                     dmap_type_char      },
    { 'asbo',     "daap.songbookmark",                     dmap_type_long      },
    { 'aspu',     "daap.songpodcasturl",                   dmap_type_string    },
    { 'asai',     "daap.songalbumid",                      dmap_type_longlong  },
    { 'asls',     "daap.songlongsize",                     dmap_type_longlong  },
    { 'asaa',     "daap.songalbumartist",                  dmap_type_string    },
    { 'aeNV',     "com.apple.itunes.norm-volume",          dmap_type_long      },
    { 'aeSP',     "com.apple.itunes.smart-playlist",       dmap_type_char      },
    { 'aeSI',     "com.apple.itunes.itms-songid",          dmap_type_long      },
    { 'aeAI',     "com.apple.itunes.itms-artistid",        dmap_type_long      },
    { 'aePI',     "com.apple.itunes.itms-playlistid",      dmap_type_long      },
    { 'aeCI',     "com.apple.itunes.itms-composerid",      dmap_type_long      },
    { 'aeGI',     "com.apple.itunes.itms-genreid",         dmap_type_long      },
    { 'aeSF',     "com.apple.itunes.itms-storefrontid",    dmap_type_long      },
    { 'aePC',     "com.apple.itunes.is-podcast",           dmap_type_char      },
    { 'aeHV',     "com.apple.itunes.has-video",            dmap_type_char      },
    { 'aeMK',     "com.apple.itunes.mediakind",            dmap_type_char      },
    { 'aeSN',     "com.apple.itunes.series-name",          dmap_type_string    },
    { 'aeNN',     "com.apple.itunes.network-name",         dmap_type_string    },
    { 'aeEN',     "com.apple.itunes.episode-num-str",      dmap_type_string    },
    { 'aeES',     "com.apple.itunes.episode-sort",         dmap_type_long      },
    { 'aeSU',     "com.apple.itunes.season-num",           dmap_type_long      },
    { 'aeGH',     "com.apple.itunes.gapless-heur",         dmap_type_long      },
    { 'aeGD',     "com.apple.itunes.gapless-enc-dr",       dmap_type_long      },
    { 'aeGU',     "com.apple.itunes.gapless-dur",          dmap_type_longlong  },
    { 'aeGR',     "com.apple.itunes.gapless-resy",         dmap_type_longlong  },
    { 'aeGE',     "com.apple.itunes.gapless-enc-del",      dmap_type_long      },
    { '\?\?\?\?', "com.apple.itunes.req-fplay",            dmap_type_char      },
    { 'aePS',     "com.apple.itunes.special-playlist",     dmap_type_char      },
    { 'aeCR',     "com.apple.itunes.content-rating",       dmap_type_string    },
    { 'aeSG',     "com.apple.itunes.saved-genius",         dmap_type_char      },
    { 'aeHD',     "com.apple.itunes.is-hd-video",          dmap_type_char      },
    { 'aeSE',     "com.apple.itunes.store-pers-id",        dmap_type_longlong  },
    { 'aeDR',     "com.apple.itunes.drm-user-id",          dmap_type_longlong  },
    { 'aeND',     "com.apple.itunes.non-drm-user-id",      dmap_type_longlong  },
    { 'aeK1',     "com.apple.itunes.drm-key1-id",          dmap_type_longlong  },
    { 'aeK2',     "com.apple.itunes.drm-key2-id",          dmap_type_longlong  },
    { 'aeDV',     "com.apple.itunes.drm-versions",         dmap_type_long      },
    { 'aeDP',     "com.apple.itunes.drm-platform-id",      dmap_type_long      },
    { 'aeXD',     "com.apple.itunes.xid",                  dmap_type_string    },
    { 'aeMk',     "com.apple.itunes.extended-media-kind",  dmap_type_long      },
    { 'aeAD',     "com.apple.itunes.adam-ids-array",       dmap_type_container },
    { 'aeSV',     "com.apple.itunes.music-sharing-version",dmap_type_long      },
    { 'cmst',     "com.airfloat.nowplayingcontainer",      dmap_type_container },
    { 'caps',     "com.airfloat.nowplayingstatus",         dmap_type_char      },
};

const uint32_t _atom_type_count = sizeof(_atom_types) / sizeof(struct dmap_atom_type);

struct dmap_t* dmap_create() {
    struct dmap_t* t = (struct dmap_t*)malloc(sizeof(struct dmap_t));
    if (t == NULL)
        return NULL;
    bzero(t, sizeof(struct dmap_t));
    return t;
}

void dmap_destroy(struct dmap_t* d) {
    if (d == NULL)
        return;
    for (uint32_t i = 0 ; i < d->count ; i++) {
        if (d->atoms[i].container != NULL)
            dmap_destroy(d->atoms[i].container);
        free(d->atoms[i].buffer);
    }
    free(d->atoms);
    free(d);
}

dmap_type dmap_type_for_tag(uint32_t tag) {
    for (uint32_t i = 0 ; i < _atom_type_count ; i++)
        if (_atom_types[i].tag == tag)
            return _atom_types[i].type;
    return dmap_type_unknown;
}

size_t _dmap_minimum_size_for_type(dmap_type type) {
    switch (type) {
        case dmap_type_char: return 1;
        case dmap_type_short: return 2;
        case dmap_type_long:
        case dmap_type_date:
        case dmap_type_version: return 4;
        case dmap_type_longlong: return 8;
        default: return 0;
    }
}

bool _dmap_parse_internal(struct dmap_t* d, const void* data, size_t data_size, uint32_t depth) {
    if (d == NULL || (data == NULL && data_size > 0) || depth > DMAP_MAX_NESTING_DEPTH)
        return false;

    const unsigned char* buffer = (const unsigned char*)data;
    size_t length = data_size;

    while (length > 0) {
        if (length < 8)
            return false;

        uint32_t frame_size = 0;
        uint32_t tag = 0;
        memcpy(&tag, buffer, sizeof(uint32_t));
        memcpy(&frame_size, buffer + 4, sizeof(uint32_t));
        tag = btml(tag);
        frame_size = btml(frame_size);

        size_t payload_length = length - 8;
        if ((size_t)frame_size > payload_length)
            return false;

        dmap_type type = dmap_type_for_tag(tag);
        if ((size_t)frame_size < _dmap_minimum_size_for_type(type))
            return false;

        struct dmap_t* container = NULL;
        if (type == dmap_type_container) {
            if (depth >= DMAP_MAX_NESTING_DEPTH)
                return false;
            container = dmap_create();
            if (container == NULL)
                return false;
            if (!_dmap_parse_internal(container, buffer + 8, frame_size, depth + 1)) {
                dmap_destroy(container);
                return false;
            }
        }

        void* atom_buffer = malloc((size_t)frame_size + 1);
        if (atom_buffer == NULL) {
            dmap_destroy(container);
            return false;
        }
        if (frame_size > 0)
            memcpy(atom_buffer, buffer + 8, frame_size);
        ((char*)atom_buffer)[frame_size] = '\0';

        struct dmap_atom* atoms = (struct dmap_atom*)realloc(d->atoms, sizeof(struct dmap_atom) * (d->count + 1));
        if (atoms == NULL) {
            free(atom_buffer);
            dmap_destroy(container);
            return false;
        }
        d->atoms = atoms;

        struct dmap_atom* atom = &d->atoms[d->count];
        bzero(atom, sizeof(struct dmap_atom));
        atom->tag = tag;
        atom->type = type;
        atom->buffer = atom_buffer;
        atom->size = frame_size;
        atom->container = container;
        d->count++;

        size_t consumed = 8 + (size_t)frame_size;
        buffer += consumed;
        length -= consumed;
    }

    return true;
}

void dmap_parse(struct dmap_t* d, const void* data, size_t data_size) {
    if (d == NULL || (data == NULL && data_size > 0))
        return;

    struct dmap_t* parsed = dmap_create();
    if (parsed == NULL)
        return;

    if (!_dmap_parse_internal(parsed, data, data_size, 0)) {
        dmap_destroy(parsed);
        return;
    }

    if (parsed->count > 0) {
        struct dmap_atom* atoms = (struct dmap_atom*)realloc(d->atoms, sizeof(struct dmap_atom) * (d->count + parsed->count));
        if (atoms == NULL) {
            dmap_destroy(parsed);
            return;
        }
        d->atoms = atoms;
        memcpy(&d->atoms[d->count], parsed->atoms, sizeof(struct dmap_atom) * parsed->count);
        d->count += parsed->count;
        free(parsed->atoms);
        parsed->atoms = NULL;
        parsed->count = 0;
    }

    dmap_destroy(parsed);
}

struct dmap_t* dmap_copy(struct dmap_t* d) {
    if (d == NULL)
        return NULL;
    struct dmap_t* ret = dmap_create();
    if (ret == NULL)
        return NULL;
    for (uint32_t i = 0 ; i < d->count ; i++) {
        struct dmap_atom* atoms = (struct dmap_atom*)realloc(ret->atoms, sizeof(struct dmap_atom) * (ret->count + 1));
        if (atoms == NULL) {
            dmap_destroy(ret);
            return NULL;
        }
        ret->atoms = atoms;
        struct dmap_atom* atom = &ret->atoms[ret->count];
        bzero(atom, sizeof(struct dmap_atom));
        atom->tag = d->atoms[i].tag;
        atom->type = d->atoms[i].type;
        atom->size = d->atoms[i].size;
        if (d->atoms[i].buffer != NULL) {
            atom->buffer = malloc(d->atoms[i].size + 1);
            if (atom->buffer == NULL) {
                dmap_destroy(ret);
                return NULL;
            }
            memcpy(atom->buffer, d->atoms[i].buffer, d->atoms[i].size);
            ((char*)atom->buffer)[d->atoms[i].size] = '\0';
        }
        if (d->atoms[i].container != NULL) {
            atom->container = dmap_copy(d->atoms[i].container);
            if (atom->container == NULL) {
                dmap_destroy(ret);
                return NULL;
            }
        }
        ret->count++;
    }
    return ret;
}

uint32_t dmap_tag_for_identifier(const char* identifier) {
    if (identifier == NULL)
        return 0;
    for (uint32_t i = 0 ; i < _atom_type_count ; i++)
        if (strcmp(_atom_types[i].identifier, identifier) == 0)
            return _atom_types[i].tag;
    if (strlen(identifier) >= 4) {
        uint32_t ret = 0;
        memcpy(&ret, identifier, sizeof(uint32_t));
        return ret;
    }
    return 0;
}

const char* dmap_identifier_for_tag(uint32_t tag) {
    for (uint32_t i = 0 ; i < _atom_type_count ; i++)
        if (_atom_types[i].tag == tag)
            return _atom_types[i].identifier;
    static char ret[sizeof(uint32_t) + 1];
    ret[sizeof(uint32_t)] = '\0';
    memcpy(ret, (char*)&tag, sizeof(uint32_t));
    return ret;
}

uint32_t dmap_get_count(struct dmap_t* d) {
    return d != NULL ? d->count : 0;
}

uint32_t dmap_get_tag_at_index(struct dmap_t* d, uint32_t index) {
    if (d == NULL || index >= d->count)
        return 0;
    return d->atoms[index].tag;
}

uint32_t dmap_get_index_of_tag(struct dmap_t* d, uint32_t tag) {
    if (d == NULL)
        return DMAP_INDEX_NOT_FOUND;
    for (uint32_t i = 0 ; i < d->count ; i++)
        if (d->atoms[i].tag == tag)
            return i;
    return DMAP_INDEX_NOT_FOUND;
}

size_t dmap_get_length(struct dmap_t* d) {
    if (d == NULL)
        return 0;
    size_t ret = 0;
    for (uint32_t i = 0 ; i < d->count ; i++) {
        if (d->atoms[i].type == dmap_type_container && d->atoms[i].container != NULL)
            ret += dmap_get_length(d->atoms[i].container) + 8;
        else
            ret += d->atoms[i].size + 8;
    }
    return ret;
}

size_t dmap_get_content(struct dmap_t* d, void* content, size_t size) {
    if (d == NULL || content == NULL)
        return 0;
    size_t write_pos = 0;
    for (uint32_t i = 0 ; i < d->count ; i++) {
        size_t atom_size = (d->atoms[i].type == dmap_type_container && d->atoms[i].container != NULL ? dmap_get_length(d->atoms[i].container) : d->atoms[i].size);
        if (write_pos > size || atom_size > size - write_pos || atom_size + 8 > size - write_pos)
            break;
        uint32_t tag = mtbl(d->atoms[i].tag);
        uint32_t a_size = mtbl((uint32_t)atom_size);
        memcpy(&((char*)content)[write_pos], &tag, 4);
        memcpy(&((char*)content)[write_pos + 4], &a_size, 4);
        write_pos += 8;
        if (d->atoms[i].type == dmap_type_container && d->atoms[i].container != NULL)
            write_pos += dmap_get_content(d->atoms[i].container, &((char*)content)[write_pos], size - write_pos);
        else if (d->atoms[i].size > 0 && d->atoms[i].buffer != NULL) {
            memcpy(&((char*)content)[write_pos], d->atoms[i].buffer, d->atoms[i].size);
            write_pos += d->atoms[i].size;
        }
    }
    return write_pos;
}

size_t dmap_get_size_of_atom_at_index(struct dmap_t* d, uint32_t index) {
    return (d != NULL && index < d->count) ? d->atoms[index].size : 0;
}

size_t dmap_get_size_of_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_get_size_of_atom_at_index(d, index) : 0;
}

size_t dmap_get_size_of_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_get_size_of_atom_tag(d, dmap_tag_for_identifier(identifier));
}

char dmap_char_at_index(struct dmap_t* d, uint32_t index) {
    if (d == NULL || index >= d->count || d->atoms[index].type != dmap_type_char || d->atoms[index].size < 1 || d->atoms[index].buffer == NULL)
        return 0;
    return *((char*)d->atoms[index].buffer);
}

char dmap_char_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_char_at_index(d, index) : 0;
}

char dmap_char_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_char_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

int16_t dmap_short_at_index(struct dmap_t* d, uint32_t index) {
    if (d == NULL || index >= d->count || d->atoms[index].type != dmap_type_short || d->atoms[index].size < 2 || d->atoms[index].buffer == NULL)
        return 0;
    int16_t value = 0;
    memcpy(&value, d->atoms[index].buffer, sizeof(value));
    return btms(value);
}

int16_t dmap_short_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_short_at_index(d, index) : 0;
}

int16_t dmap_short_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_short_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

int32_t dmap_long_at_index(struct dmap_t* d, uint32_t index) {
    if (d == NULL || index >= d->count ||
        (d->atoms[index].type != dmap_type_long && d->atoms[index].type != dmap_type_date) ||
        d->atoms[index].size < 4 || d->atoms[index].buffer == NULL)
        return 0;
    int32_t value = 0;
    memcpy(&value, d->atoms[index].buffer, sizeof(value));
    return btml(value);
}

int32_t dmap_long_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_long_at_index(d, index) : 0;
}

int32_t dmap_long_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_long_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

int64_t dmap_longlong_at_index(struct dmap_t* d, uint32_t index) {
    if (d == NULL || index >= d->count || d->atoms[index].type != dmap_type_longlong || d->atoms[index].size < 8 || d->atoms[index].buffer == NULL)
        return 0;
    int64_t value = 0;
    memcpy(&value, d->atoms[index].buffer, sizeof(value));
    return btmll(value);
}

int64_t dmap_longlong_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_longlong_at_index(d, index) : 0;
}

int64_t dmap_longlong_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_longlong_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

const char* dmap_string_at_index(struct dmap_t* d, uint32_t index) {
    if (d == NULL || index >= d->count || d->atoms[index].type != dmap_type_string || d->atoms[index].buffer == NULL)
        return NULL;
    return (char*)d->atoms[index].buffer;
}

const char* dmap_string_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_string_at_index(d, index) : NULL;
}

const char* dmap_string_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_string_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

uint32_t dmap_date_at_index(struct dmap_t* d, uint32_t index) {
    return (uint32_t)dmap_long_at_index(d, index);
}

uint32_t dmap_date_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    return (uint32_t)dmap_long_for_atom_tag(d, tag);
}

uint32_t dmap_date_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return (uint32_t)dmap_long_for_atom_identifer(d, identifier);
}

dmap_version dmap_version_at_index(struct dmap_t* d, uint32_t index) {
    dmap_version version = { 0, 0, 0 };
    if (d == NULL || index >= d->count || d->atoms[index].type != dmap_type_version || d->atoms[index].size < sizeof(dmap_version) || d->atoms[index].buffer == NULL)
        return version;
    memcpy(&version, d->atoms[index].buffer, sizeof(version));
    version.major = btms(version.major);
    return version;
}

dmap_version dmap_version_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_version_at_index(d, index) : (dmap_version){ 0, 0, 0 };
}

dmap_version dmap_version_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_version_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

struct dmap_t* dmap_container_at_index(struct dmap_t* d, uint32_t index) {
    if (d == NULL || index >= d->count || d->atoms[index].type != dmap_type_container)
        return NULL;
    return d->atoms[index].container;
}

struct dmap_t* dmap_container_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_container_at_index(d, index) : NULL;
}

struct dmap_t* dmap_container_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_container_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

const void* dmap_bytes_at_index(struct dmap_t* d, uint32_t index) {
    return (d != NULL && index < d->count) ? d->atoms[index].buffer : NULL;
}

const void* dmap_bytes_for_atom_tag(struct dmap_t* d, uint32_t tag) {
    uint32_t index = dmap_get_index_of_tag(d, tag);
    return index != DMAP_INDEX_NOT_FOUND ? dmap_bytes_at_index(d, index) : NULL;
}

const void* dmap_bytes_for_atom_identifer(struct dmap_t* d, const char* identifier) {
    return dmap_bytes_for_atom_tag(d, dmap_tag_for_identifier(identifier));
}

struct dmap_atom* _add_atom(struct dmap_t* d, uint32_t tag, uint32_t type) {
    if (d == NULL)
        return NULL;
    struct dmap_atom* atoms = (struct dmap_atom*)realloc(d->atoms, sizeof(struct dmap_atom) * (d->count + 1));
    if (atoms == NULL)
        return NULL;
    d->atoms = atoms;
    struct dmap_atom* ret = &d->atoms[d->count];
    d->count++;
    bzero(ret, sizeof(struct dmap_atom));
    ret->tag = tag;
    ret->type = type;
    return ret;
}

bool _set_atom_buffer(struct dmap_atom* atom, const void* buffer, size_t size) {
    if (atom == NULL || (buffer == NULL && size > 0))
        return false;
    void* replacement = malloc(size + 1);
    if (replacement == NULL)
        return false;
    bzero(replacement, size + 1);
    if (size > 0)
        memcpy(replacement, buffer, size);
    free(atom->buffer);
    atom->buffer = replacement;
    atom->size = size;
    return true;
}

void dmap_add_char(struct dmap_t* d, int8_t chr, uint32_t tag) {
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_char);
    _set_atom_buffer(new_atom, &chr, sizeof(int8_t));
}

void dmap_add_short(struct dmap_t* d, int16_t shrt, uint32_t tag) {
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_short);
    int16_t val = mtbs(shrt);
    _set_atom_buffer(new_atom, &val, sizeof(int16_t));
}

void dmap_add_long(struct dmap_t* d, int32_t lng, uint32_t tag) {
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_long);
    int32_t val = mtbl(lng);
    _set_atom_buffer(new_atom, &val, sizeof(int32_t));
}

void dmap_add_longlong(struct dmap_t* d, int64_t lnglng, uint32_t tag) {
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_longlong);
    int64_t val = mtbll(lnglng);
    _set_atom_buffer(new_atom, &val, sizeof(int64_t));
}

void dmap_add_string(struct dmap_t* d, const char* string, uint32_t tag) {
    if (string == NULL)
        return;
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_string);
    _set_atom_buffer(new_atom, string, strlen(string));
}

void dmap_add_date(struct dmap_t* d, uint32_t date, uint32_t tag) {
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_date);
    uint32_t val = mtbl(date);
    _set_atom_buffer(new_atom, &val, sizeof(uint32_t));
}

void dmap_add_version(struct dmap_t* d, dmap_version version, uint32_t tag) {
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_version);
    dmap_version val = version;
    val.major = mtbs(val.major);
    _set_atom_buffer(new_atom, &val, sizeof(dmap_version));
}

void dmap_add_container(struct dmap_t* d, struct dmap_t* container, uint32_t tag) {
    if (container == NULL)
        return;
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_container);
    if (new_atom != NULL)
        new_atom->container = dmap_copy(container);
}

void dmap_add_bytes(struct dmap_t* d, const void* data, size_t size, uint32_t tag) {
    struct dmap_atom* new_atom = _add_atom(d, tag, dmap_type_unknown);
    _set_atom_buffer(new_atom, data, size);
}
