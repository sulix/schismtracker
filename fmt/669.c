/*
 * Schism Tracker - a cross-platform Impulse Tracker clone
 * copyright (c) 2003-2005 Storlek <storlek@rigelseven.com>
 * copyright (c) 2005-2008 Mrs. Brisby <mrs.brisby@nimh.org>
 * copyright (c) 2009 Storlek & Mrs. Brisby
 * copyright (c) 2010-2011 Storlek
 * URL: http://schismtracker.org/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define NEED_BYTESWAP
#include "headers.h"
#include "slurp.h"
#include "fmt.h"

#include "sndfile.h"

/* --------------------------------------------------------------------- */

struct header_669 {
        char sig[2];
        char songmessage[108];
        uint8_t samples;
        uint8_t patterns;
        uint8_t restartpos;
        uint8_t orders[128];
        uint8_t tempolist[128];
        uint8_t breaks[128];
};

int fmt_669_read_info(dmoz_file_t *file, const uint8_t *data, size_t length)
{
        struct header_669 *header = (struct header_669 *) data;
        unsigned long i;
        const char *desc;

        if (length < sizeof(struct header_669))
                return 0;

        /* Impulse Tracker identifies any 669 file as a "Composer 669 Module",
        regardless of the signature tag. */
        if (memcmp(header->sig, "if", 2) == 0)
                desc = "Composer 669 Module";
        else if (memcmp(header->sig, "JN", 2) == 0)
                desc = "Extended 669 Module";
        else
                return 0;

        if (header->samples == 0 || header->patterns == 0
            || header->samples > 64 || header->patterns > 128
            || header->restartpos > 127)
                return 0;
        for (i = 0; i < 128; i++)
                if (header->breaks[i] > 0x3f)
                        return 0;

        /* From my very brief observation, it seems the message of a 669 file is split into 3 lines.
        This (naively) takes the first line of it as the title, as the format doesn't actually have
        a field for a song title. */
        file->title = (char *) calloc(37, sizeof(char));
        memcpy(file->title, header->songmessage, 36);
        file->title[36] = 0;

        file->description = desc;
        /*file->extension = str_dup("669");*/
        file->type = TYPE_MODULE_S3M;

        return 1;
}

/* --------------------------------------------------------------------------------------------------------- */

/* <opinion humble="false">This is better than IT's and MPT's 669 loaders</opinion> */

int fmt_669_load_song(song_t *song, slurp_t *fp, unsigned int lflags)
{
        uint8_t b[16];
        uint16_t npat, nsmp;
        int n, pat, chan, smp, row;
        song_note_t *note;
        uint16_t tmp;
        uint32_t tmplong;
        uint8_t patspeed[128], breakpos[128];
        uint8_t restartpos;
        const char *tid;
        char titletmp[37];

        slurp_read(fp, &tmp, 2);
        switch (bswapLE16(tmp)) {
        case 0x6669: // 'if'
                tid = "Composer 669";
                break;
        case 0x4e4a: // 'JN'
                tid = "UNIS 669";
                break;
        default:
                return LOAD_UNSUPPORTED;
        }

        /* The message is 108 bytes, split onto 3 lines of 36 bytes each.
        Also copy the first part of the message into the title, because 669 doesn't actually have
        a dedicated title field... */
        read_lined_message(song->message, fp, 108, 36);
        strncpy(titletmp, song->message, 36);
        titletmp[36] = '\0';
        titletmp[strcspn(titletmp, "\r\n")] = '\0';
        trim_string(titletmp);
        titletmp[25] = '\0';
        strcpy(song->title, titletmp);

        nsmp = slurp_getc(fp);
        npat = slurp_getc(fp);
        restartpos = slurp_getc(fp);

        if (nsmp > 64 || npat > 128 || restartpos > 127)
                return LOAD_UNSUPPORTED;

        strcpy(song->tracker_id, tid);

        /* orderlist */
        slurp_read(fp, song->orderlist, 128);

        /* stupid crap */
        slurp_read(fp, patspeed, 128);
        slurp_read(fp, breakpos, 128);

        /* samples */
        for (smp = 1; smp <= nsmp; smp++) {
                slurp_read(fp, b, 13);
                b[13] = 0; /* the spec says it's supposed to be ASCIIZ, but some 669's use all 13 chars */
                strcpy(song->samples[smp].name, (char *) b);
                b[12] = 0; /* ... filename field only has room for 12 chars though */
                strcpy(song->samples[smp].filename, (char *) b);

                slurp_read(fp, &tmplong, 4);
                song->samples[smp].length = bswapLE32(tmplong);
                slurp_read(fp, &tmplong, 4);
                song->samples[smp].loop_start = bswapLE32(tmplong);
                slurp_read(fp, &tmplong, 4);
                tmplong = bswapLE32(tmplong);
                if (tmplong > song->samples[smp].length)
                        tmplong = 0;
                else
                        song->samples[smp].flags |= CHN_LOOP;
                song->samples[smp].loop_end = tmplong;

                song->samples[smp].c5speed = 8363;
                song->samples[smp].volume = 60;  /* ickypoo */
                song->samples[smp].volume *= 4; //mphack
                song->samples[smp].global_volume = 64;  /* ickypoo */
                song->samples[smp].vib_type = 0;
                song->samples[smp].vib_rate = 0;
                song->samples[smp].vib_depth = 0;
                song->samples[smp].vib_speed = 0;
        }

        /* patterns */
        for (pat = 0; pat < npat; pat++) {
                uint8_t effect[8] = {
                        255, 255, 255, 255, 255, 255, 255, 255
                };
                uint8_t rows = breakpos[pat] + 1;

                note = song->patterns[pat] = csf_allocate_pattern(CLAMP(rows, 32, 64));
                song->pattern_size[pat] = song->pattern_alloc_size[pat] = CLAMP(rows, 32, 64);

                for (row = 0; row < rows; row++, note += 56) {
                        for (chan = 0; chan < 8; chan++, note++) {
                                slurp_read(fp, b, 3);

                                switch (b[0]) {
                                case 0xfe:     /* no note, only volume */
                                        note->voleffect = VOLFX_VOLUME;
                                        note->volparam = (b[1] & 0xf) << 2;
                                        break;
                                case 0xff:     /* no note or volume */
                                        break;
                                default:
                                        note->note = (b[0] >> 2) + 36 + 1;
                                        note->instrument = ((b[0] & 3) << 4 | (b[1] >> 4)) + 1;
                                        note->voleffect = VOLFX_VOLUME;
                                        note->volparam = (b[1] & 0xf) << 2;
                                        break;
                                }
                                /* (sloppily) import the stupid effect */
                                if (b[2] != 0xff)
                                        effect[chan] = b[2];
                                if (effect[chan] == 0xff)
                                        continue;
                                note->param = effect[chan] & 0xf;
                                switch (effect[chan] >> 4) {
                                default:
                                        /* oops. never mind. */
                                        //printf("ignoring effect %X\n", effect[chan]);
                                        note->param = 0;
                                        break;
                                case 0: /* A - portamento up */
                                        note->effect = FX_PORTAMENTOUP;
                                        break;
                                case 1: /* B - portamento down */
                                        note->effect = FX_PORTAMENTODOWN;
                                        break;
                                case 2: /* C - port to note */
                                        note->effect = FX_TONEPORTAMENTO;
                                        break;
                                case 3: /* D - frequency adjust (??) */
                                        note->effect = FX_PORTAMENTODOWN;
                                        if (note->param)
                                                note->param |= 0xf0;
                                        else
                                                note->param = 0xf1;
                                        effect[chan] = 0xff;
                                        break;
                                case 4: /* E - frequency vibrato */
                                        note->effect = FX_VIBRATO;
                                        note->param |= 0x80;
                                        break;
                                case 5: /* F - set tempo */
                                        /* TODO: param 0 is a "super fast tempo" in extended mode (?) */
                                        if (note->param)
                                                note->effect = FX_SPEED;
                                        effect[chan] = 0xff;
                                        break;
                                case 6: /* G - subcommands (extended) */
                                        switch (note->param) {
                                        case 0: /* balance fine slide left */
                                                //TODO("test pan slide effect (P%dR%dC%d)", pat, row, chan);
                                                note->effect = FX_PANNINGSLIDE;
                                                note->param = 0x8F;
                                                break;
                                        case 1: /* balance fine slide right */
                                                //TODO("test pan slide effect (P%dR%dC%d)", pat, row, chan);
                                                note->effect = FX_PANNINGSLIDE;
                                                note->param = 0xF8;
                                                break;
                                        default:
                                                /* oops, nothing again */
                                                note->param = 0;
                                        }
                                        break;
                                case 7: /* H - slot retrig */
                                        //TODO("test slot retrig (P%dR%dC%d)", pat, row, chan);
                                        note->effect = FX_RETRIG;
                                        break;
                                }
                        }
                }
                if (rows < 64) {
                        /* skip the rest of the rows beyond the break position */
                        slurp_seek(fp, 3 * 8 * (64 - rows), SEEK_CUR);
                }

                /* handle the stupid pattern speed */
                note = song->patterns[pat];
                for (chan = 0; chan < 9; chan++, note++) {
                        if (note->effect == FX_SPEED) {
                                break;
                        } else if (!note->effect) {
                                note->effect = FX_SPEED;
                                note->param = patspeed[pat];
                                break;
                        }
                }
                /* handle the break position */
                if (rows < 32) {
                        //printf("adding pattern break for pattern %d\n", pat);
                        note = song->patterns[pat] + MAX_CHANNELS * (rows - 1);
                        for (chan = 0; chan < 9; chan++, note++) {
                                if (!note->effect) {
                                        note->effect = FX_PATTERNBREAK;
                                        note->param = 0;
                                        break;
                                }
                        }
                }
        }
        csf_insert_restart_pos(song, restartpos);

        /* sample data */
        if (!(lflags & LOAD_NOSAMPLES)) {
                for (smp = 1; smp <= nsmp; smp++) {
                        uint32_t ssize;

                        if (song->samples[smp].length == 0)
                                continue;

                        ssize = csf_read_sample(song->samples + smp, SF_LE | SF_M | SF_PCMU | SF_8,
                                fp->data + fp->pos, fp->length - fp->pos);
                        slurp_seek(fp, ssize, SEEK_CUR);
                }
        }

        /* set the rest of the stuff */
        song->initial_speed = 4;
        song->initial_tempo = 78;
        song->flags = SONG_ITOLDEFFECTS | SONG_LINEARSLIDES;

        song->pan_separation = 64;
        for (n = 0; n < 8; n++)
                song->channels[n].panning = (n & 1) ? 256 : 0; //mphack
        for (n = 8; n < 64; n++)
                song->channels[n].flags = CHN_MUTE;

//      if (ferror(fp)) {
//              return LOAD_FILE_ERROR;
//      }

        /* done! */
        return LOAD_SUCCESS;
}

