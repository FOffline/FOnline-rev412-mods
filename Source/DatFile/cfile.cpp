/*
 * fDAT Plug-in source code: CFile class and its descendants
 *
 * Copyright (C) 2000 Alexander Belyakov
 * E-mail: abel@krasu.ru
 *
 * Changed to be used in Fallout Mapper.
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
    05 Sep 2002, ABel:
        C file-handle functions replaced by native Windows API ones
*/

#include "cfile.h"
#include "unlzss.h"


long CPlainFile::seek (long dist, int from) {
	long absPos;
	if (from == SEEK_CUR)
		absPos = tell() + dist;
	else if (from == SEEK_END)
		absPos = fileSize + dist;
	else if (from == SEEK_SET)
		absPos = dist;
	else
		return 0xFFFFFFFF;

	if (absPos < 0)
		absPos = 0;
	else if (absPos > fileSize)
		absPos = fileSize;
	long res = FileSetPointer (hFile, absPos + beginPos, SEEK_SET);
	if (res != (long)0xFFFFFFFF)
		res -= beginPos;
	return res;
}
int CPlainFile::read (void* buf, long toRead, long* read) {
	long left = fileSize - tell();
	if (left < toRead)
		toRead = left;
	FileRead (hFile, buf, toRead, (uint*)read);
	return (*read != -1);
}


long CPackedFile::seek (long dist, int from) {
	if (from == SEEK_CUR) {
		if (!skipper) skipper = new uchar [BUFF_SIZE];
		if (dist > 0)
			skip (dist);
		else if (dist < 0) {
			dist = curPos + dist;
			reset();
			skip (dist);
		}
	} else if (from == SEEK_END) {
		if (dist < 0) {
			if (!skipper) skipper = new uchar [BUFF_SIZE];
			if (curPos > dist)
				reset();
			skip (dist - curPos);
		} else
			curPos = fileSize;
	} else {
		if (dist < 0)
			reset();
		else {
			if (!skipper) skipper = new uchar [BUFF_SIZE];
			if (curPos > dist)
				reset();
			skip (dist - curPos);
		}
	}
	return curPos;
}
void CPackedFile::skip (long dist) {
	if (!dist)
		return;
	if (curPos + dist >= fileSize) {
		curPos = fileSize;
		return;
	}
	long res;
	while (dist > 0) {
		res = (dist > BUFF_SIZE)? BUFF_SIZE: dist;
		if (!read (skipper, res, &res) || !res)
			break;
		dist -= res;
	}
}


int C_Z_PackedFile::read (void* buf, long toRead, long* read) {
	int res = Z_OK;

	if (!buf) return 0;
	if (!toRead || curPos >= fileSize) {
		*read = 0;
		return 1;
	}

	if (!inBuf) inBuf = new uchar [BUFF_SIZE];

	stream. next_out = (uchar*)buf;
	stream. avail_out = toRead;

	long oldTotOut = stream. total_out;

	long left = packedSize + beginPos - FileSetPointer(hFile, 0, SEEK_CUR);

	while (stream. avail_out && res == Z_OK) {
		if ((!stream. avail_in) && left > 0) {
			stream. next_in = inBuf;
			FileRead (hFile, inBuf, (left>BUFF_SIZE)?BUFF_SIZE:left, (uint*)&stream. avail_in);
			left -= stream. avail_in;
		}
		unsigned long progress = stream. total_out;
		res = inflate (&stream, Z_NO_FLUSH);
		if (progress == stream. total_out && left <= 0)
			break;
	}
	*read = stream. total_out - oldTotOut;
	curPos += *read;

	if (!(*read))
		fileSize = curPos; // looks like a joke, but it is not

	return (res == Z_OK || res == Z_STREAM_END);
}


// binary search
int findIndex (long pos, block* blocks, int count) {
	if (pos < 0 || !blocks)
		return -1;
	int b = 0, e = count, m;
	int ob, oe;
	int i = -1;
	while (i == -1) {
		m = (b + e)/2;
		ob = b; oe = e;
		if ( blocks[m]. filePos <= pos && pos < blocks[m+1]. filePos )
			i = m;
		if (pos < blocks[m]. filePos)
			e = m - 1;
		else if (pos >= blocks[m+1]. filePos)
			b = m + 1;
		if ( b < ob || b > oe || e < ob || e > oe)
			break;
	}
	return i;
}

#ifdef USE_LZ_BLOCKS
void C_LZ_BlockFile::skip (long dist) {
	if (!dist)
		return;
	long absPos = curPos + dist;
	if (absPos >= fileSize) {
		curPos = fileSize;
		return;
	}

	if (!blocks)
		allocateBlocks();

	int block = -1;
	if (absPos < blocks[knownBlocks-1]. filePos)
		block = findIndex (absPos, blocks, knownBlocks-1);
	else
		block = knownBlocks - 1;

	if (block == -1)
		return;
	if (block != currentBlock-1) {
		decompressor->clear();
		FileSetPointer (hFile, blocks[block]. archPos, SEEK_SET);
		dist = absPos - blocks[block]. filePos;
		currentBlock = block;
		curPos = blocks[block]. filePos;
	}
	while (dist>0) {
		long res;
		if (!read (skipper, (dist>BUFF_SIZE)? BUFF_SIZE: dist, &res) || !res)
			break;
		dist -= res;
	}
}
#endif
int C_LZ_BlockFile::read (void* buf, long toRead, long* read) {
	if (!buf) return 0;
	if (!toRead || curPos >= fileSize) {
		*read = 0;
		return 1;
	}

	if (!inBuf) inBuf = new uchar [BUFF_SIZE];
#ifdef USE_LZ_BLOCKS
	if (!blocks)
		allocateBlocks();
#endif

	long res = 0,
		tot = 0;

	while (toRead && curPos < fileSize) {
		if ( !decompressor->left() ) {
			unsigned short lhdr;
			uint read_cnt;
			FileRead (hFile, &lhdr, 2);
			lhdr = ((lhdr & 0xFF00) >> 8) + ((lhdr & 0x00FF) << 8);
            FileRead (hFile, inBuf, lhdr & 0x7FFF, &read_cnt);
			decompressor->takeNewData (inBuf, read_cnt,	(lhdr & 0x8000) == 0);
#ifdef USE_LZ_BLOCKS
			currentBlock++;
			if (currentBlock == knownBlocks) {
				blocks [currentBlock]. filePos = blocks [currentBlock - 1]. filePos + decompressor->left();
				blocks [currentBlock]. archPos = blocks [currentBlock - 1]. archPos + (lhdr & 0x7FFF) + 2;
				knownBlocks++;
			}
#endif
		}

		res = decompressor->getUnpacked ((unsigned char*)buf, toRead);
		if (!res)
			break;
		buf = (unsigned char*)buf + res;
		curPos += res;
		tot += res;
		toRead -= res;
	}

	*read = tot;
	if (!(*read))
		fileSize = curPos;
	return 1;
}
#ifdef USE_LZ_BLOCKS
void C_LZ_BlockFile::allocateBlocks() {
	long oldFilePos = FileSetPointer(hFile, 0, SEEK_CUR);

// get count of blocks:
	long blockCnt = 0,
		blocksSize = 0;
	unsigned short lhdr;
	FileSetPointer (hFile, beginPos, SEEK_SET);
	while (blocksSize < packedSize) {
		FileRead (hFile, &lhdr, 2);
		lhdr = ((lhdr & 0xFF00) >> 8) + ((lhdr & 0x007F) << 8);
		blocksSize += lhdr;
		blockCnt++;
		FileSetPointer (hFile, lhdr, SEEK_CUR);
	}

	blocks = new block [blockCnt + 1];
	blocks[0]. filePos = 0;
	blocks[0]. archPos = beginPos;
	knownBlocks = 1;

	FileSetPointer (hFile, oldFilePos, SEEK_SET);
}
#endif
