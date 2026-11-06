#if defined(__APPLE__)
#define _DARWIN_USE_64_BIT_INODE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <iostream>
#include <exception>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <inttypes.h>
#include <errno.h>
#include <stdexcept>

#include "udpbd.h"

#define BUFLEN  2048

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/ioctl.h>
#include <sys/disk.h>
#endif

using namespace std;

/*
 * class CBlockDevice
 */
class CBlockDevice
{
public:
    CBlockDevice(const char *sFileName, bool verbose) : _read_only(false), _verbose(verbose) {
        // Open the selected file. This file will be used as the Block Device.
        _fp = open(sFileName, _read_only ? O_RDONLY : O_RDWR);
        if (_fp < 0) {
            _read_only = true;
            _fp = open(sFileName, O_RDONLY);
            if (_fp < 0)
                throw runtime_error(string("unable to open file ") + sFileName);
        }

        // Get the size of the file. off_t is 64-bit on 64-bit targets and
        // _FILE_OFFSET_BITS makes it 64-bit on 32-bit glibc targets too.
        _fsize = lseek(_fp, 0, SEEK_END);
        if (_fsize < 0)
            throw runtime_error(string("unable to determine size of ") + sFileName + ": " + strerror(errno));
        if (lseek(_fp, 0, SEEK_SET) < 0)
            throw runtime_error(string("unable to seek ") + sFileName + ": " + strerror(errno));
#if defined(__APPLE__)
        if (_fsize == 0) {
            uint64_t blockCount;
            uint32_t blockSize;
            ioctl(_fp, DKIOCGETBLOCKCOUNT, &blockCount);
            ioctl(_fp, DKIOCGETBLOCKSIZE, &blockSize);
            _fsize = blockCount * blockSize;
        }
#elif defined(__FreeBSD__)
        if (_fsize == 0) {
            uint64_t mediaSize;
            ioctl(_fp, DIOCGMEDIASIZE, &mediaSize);
            _fsize = mediaSize;
        }
#endif

        if (_verbose) {
            printf("Opened '%s' as Block Device\n", sFileName);
            printf(" - %s\n", _read_only ? "read-only" : "read/write");
            printf(" - size = %" PRIdMAX "MB / %" PRIdMAX "MiB\n",
                   static_cast<intmax_t>(_fsize / (1000 * 1000)),
                   static_cast<intmax_t>(_fsize / (1024 * 1024)));
        }
    }

    ~CBlockDevice() {
        close(_fp);
    }

    void read_at(uint64_t offset, void *data, size_t size) {
        char *out = static_cast<char *>(data);
        size_t done = 0;
        while (done < size) {
            ssize_t rv = pread(_fp, out + done, size - done, static_cast<off_t>(offset + done));
            if (rv < 0) {
                if (errno == EINTR)
                    continue;
                throw runtime_error(string("read failed: ") + strerror(errno));
            }
            if (rv == 0)
                throw runtime_error("unexpected end of block device while reading");
            done += static_cast<size_t>(rv);
        }
    }

    void write_at(uint64_t offset, const void *data, size_t size) {
        const char *in = static_cast<const char *>(data);
        size_t done = 0;
        while (done < size) {
            ssize_t rv = pwrite(_fp, in + done, size - done, static_cast<off_t>(offset + done));
            if (rv < 0) {
                if (errno == EINTR)
                    continue;
                throw runtime_error(string("write failed: ") + strerror(errno));
            }
            if (rv == 0)
                throw runtime_error("write made no progress");
            done += static_cast<size_t>(rv);
        }
    }

    uint32_t get_sector_size()  {return 512;}
    uint32_t get_sector_count() {return _fsize/512;}

private:
    int _fp;
    bool _read_only;
    bool _verbose;
    off_t _fsize;
};

/*
 * class CUDPBDServer
 */
class CUDPBDServer
{
public:
    CUDPBDServer(class CBlockDevice &bd, bool verbose) : _bd(bd), _verbose(verbose), _block_shift(0), _total_read(0), _total_write(0), _write_size_left(0), _write_offset(0) {
        set_block_shift(5); // 128-byte blocks; protocol block sizing is unchanged.
        struct sockaddr_in si_me;

        // Create a UDP socket.
        if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
            throw runtime_error("socket");
        }

        // Bind socket to port.
        memset((char *) &si_me, 0, sizeof(si_me));
        si_me.sin_family = AF_INET;
        si_me.sin_port = htons(UDPBD_PORT);
        si_me.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s, (struct sockaddr*)&si_me, sizeof(si_me) ) == -1) {
            close(s);
            throw runtime_error("bind");
        }

        // Enable broadcasts.
        int broadcastEnable=1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    }

    ~CUDPBDServer() {
        close(s);
    }

    void run() {
        struct sockaddr_in si_other;
        ssize_t recv_len;
        char buf[BUFLEN];

        printf("Server running on port %d (0x%x)\n", UDPBD_PORT, UDPBD_PORT);

        // Start server loop.
        while (1) {
            socklen_t slen = sizeof(si_other);
            if ((recv_len = recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *) &si_other, &slen)) == -1) {
                if (errno == EINTR)
                    continue;
                throw runtime_error(string("recvfrom: ") + strerror(errno));
            }

            // Ignore truncated or undersized command packets instead of reading
            // incomplete structures from the receive buffer.
            if (recv_len < static_cast<ssize_t>(sizeof(struct SUDPBDv2_Header)))
                continue;

            struct SUDPBDv2_Header *hdr = (struct SUDPBDv2_Header *)buf;

            switch (hdr->cmd) {
                case UDPBD_CMD_INFO:
                    if (recv_len >= static_cast<ssize_t>(sizeof(struct SUDPBDv2_InfoRequest)))
                        handle_cmd_info(si_other, (struct SUDPBDv2_InfoRequest *)buf);
                    break;
                case UDPBD_CMD_READ:
                    if (recv_len >= static_cast<ssize_t>(sizeof(struct SUDPBDv2_RWRequest)))
                        handle_cmd_read(si_other, (struct SUDPBDv2_RWRequest *)buf);
                    break;
                case UDPBD_CMD_WRITE:
                    if (recv_len >= static_cast<ssize_t>(sizeof(struct SUDPBDv2_RWRequest)))
                        handle_cmd_write(si_other, (struct SUDPBDv2_RWRequest *)buf);
                    break;
                case UDPBD_CMD_WRITE_RDMA:
                    if (recv_len >= static_cast<ssize_t>(sizeof(struct SUDPBDv2_Header) + sizeof(union block_type)))
                        handle_cmd_write_rdma(si_other, (struct SUDPBDv2_RDMA *)buf, static_cast<size_t>(recv_len));
                    break;
                default:
                    if (_verbose)
                        printf("Invalid cmd: 0x%x\n", hdr->cmd);
            };
        }
    }

private:
    void print_stats() {
        if (_verbose) {
            printf("Total read: %" PRIu64 " KiB, total write: %" PRIu64 " KiB", _total_read/1024, _total_write/1024);
            fflush(stdout);
        }
    }

    void set_block_shift(uint32_t shift) {
        if (shift != _block_shift) {
            _block_shift       = shift;
            _block_size        = 1 << (_block_shift + 2);
            _blocks_per_packet = RDMA_MAX_PAYLOAD / _block_size;
            _blocks_per_sector = _bd.get_sector_size() / _block_size;
            if (_verbose)
                printf("Block size changed to %u\n", _block_size);
        }
    }

    void set_block_shift_sectors(uint32_t sectors) {
        // Optimize for the least number of network packets, then the largest block size.
        uint32_t shift;
        uint32_t size = sectors * 512;
        uint32_t packetsMIN  = (size + 1440 - 1) / 1440;
        uint32_t packets128 = (size + 1408 - 1) / 1408;
        uint32_t packets256 = (size + 1280 - 1) / 1280;
        uint32_t packets512 = (size + 1024 - 1) / 1024;

        if (packets512 == packetsMIN)
            shift = 7; // 512 byte blocks
        else if (packets256 == packetsMIN)
            shift = 6; // 256 byte blocks
        else if (packets128 == packetsMIN)
            shift = 5; // 128 byte blocks
        else
            shift = 3; // 32 byte blocks

        set_block_shift(shift);
    }

    void handle_cmd_info(struct sockaddr_in &si_other, struct SUDPBDv2_InfoRequest *request) {
        struct SUDPBDv2_InfoReply reply;

        if (_verbose) {
            char str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &si_other.sin_addr, str, INET_ADDRSTRLEN);
            printf("\rUDPBD_CMD_INFO from %s\n", str);
            print_stats();
        }

        reply.hdr.cmd      = UDPBD_CMD_INFO_REPLY;
        reply.hdr.cmdid    = request->hdr.cmdid;
        reply.hdr.cmdpkt   = 1;
        reply.sector_size  = _bd.get_sector_size();
        reply.sector_count = _bd.get_sector_count();

        if (sendto(s, &reply, sizeof(reply), 0, (struct sockaddr*) &si_other, sizeof(si_other)) == -1)
            throw runtime_error(string("sendto: ") + strerror(errno));
    }

    void handle_cmd_read(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request) {
        struct SUDPBDv2_RDMA reply;

        if (_verbose)
            printf("\rUDPBD_CMD_READ(cmdId=%d, startSector=%u, sectorCount=%u)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        // Optimize RDMA block size for number of sectors. The existing protocol
        // block-size selection is deliberately unchanged.
        set_block_shift_sectors(request->sector_count);

        reply.hdr.cmd        = UDPBD_CMD_READ_RDMA;
        reply.hdr.cmdid      = request->hdr.cmdid;
        reply.hdr.cmdpkt     = 1;
        reply.bt.block_shift = _block_shift;

        uint32_t blocks_left = request->sector_count * _blocks_per_sector;
        uint64_t read_offset = static_cast<uint64_t>(request->sector_nr) * 512;

        _total_read += static_cast<uint64_t>(blocks_left) * _block_size;
        print_stats();

        while (blocks_left > 0) {
            reply.bt.block_count = (blocks_left > _blocks_per_packet) ? _blocks_per_packet : blocks_left;
            blocks_left -= reply.bt.block_count;

            size_t data_size = reply.bt.block_count * _block_size;
            _bd.read_at(read_offset, reply.data, data_size);
            read_offset += data_size;

            if (sendto(s, &reply, sizeof(struct SUDPBDv2_Header) + 4 + data_size, 0, (struct sockaddr*) &si_other, sizeof(si_other)) == -1)
                throw runtime_error(string("sendto: ") + strerror(errno));
            reply.hdr.cmdpkt++;
        }
    }

    void handle_cmd_write(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request) {
        (void)si_other;
        if (_verbose)
            printf("\rUDPBD_CMD_WRITE(cmdId=%d, startSector=%u, sectorCount=%u)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        _write_offset = static_cast<uint64_t>(request->sector_nr) * 512;
        _write_size_left = request->sector_count * 512;

        _total_write += _write_size_left;
        print_stats();
    }

    void handle_cmd_write_rdma(struct sockaddr_in &si_other, struct SUDPBDv2_RDMA *request, size_t packet_size) {
        size_t size = request->bt.block_count * (1 << (request->bt.block_shift + 2));
        const size_t header_size = sizeof(struct SUDPBDv2_Header) + sizeof(union block_type);
        if (size > packet_size - header_size || size > _write_size_left) {
            if (_verbose)
                fprintf(stderr, "Ignoring invalid or unexpected write data packet\n");
            return;
        }

        _bd.write_at(_write_offset, request->data, size);
        _write_offset += size;
        _write_size_left -= size;
        if (_write_size_left == 0) {
            struct SUDPBDv2_WriteDone reply;

            reply.hdr.cmd      = UDPBD_CMD_WRITE_DONE;
            reply.hdr.cmdid    = request->hdr.cmdid;
            reply.hdr.cmdpkt   = request->hdr.cmdid + 1;
            reply.result       = 0;

            if (sendto(s, &reply, sizeof(reply), 0, (struct sockaddr*) &si_other, sizeof(si_other)) == -1)
                throw runtime_error(string("sendto: ") + strerror(errno));
        }
    }

    class CBlockDevice &_bd;
    bool _verbose;
    uint32_t _block_shift;
    uint32_t _block_size;
    uint32_t _blocks_per_packet;
    uint32_t _blocks_per_sector;
    int s;

    uint64_t _total_read;
    uint64_t _total_write;

    uint32_t _write_size_left;
    uint64_t _write_offset;
};

int main(int argc, char * argv[])
{
    const char *image = NULL;
    bool verbose = true;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0)
            verbose = false;
        else if (image == NULL)
            image = argv[i];
        else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            image = NULL;
            break;
        }
    }

    if (image == NULL) {
        printf("Usage:\n");
        printf("  %s [--quiet|-q] <file>\n", argv[0]);
        return -1;
    }

    try {
        class CBlockDevice bd(image, verbose);
        class CUDPBDServer srv(bd, verbose);
        srv.run();
    } catch (exception& e) {
        cerr << e.what() << '\n';
        return -2;
    }

    return 0;
}
