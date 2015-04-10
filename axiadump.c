#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h> 
#include <aacenc_lib.h>

int config_livewire_socket(char *multicastAddr)
{
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  int reuse = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) == -1) {
      fprintf(stderr, "setsockopt: %d\n", errno);
      return -1;
  }
  
  struct sockaddr_in addr; 
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(5004);
  addr.sin_addr.s_addr = inet_addr(multicastAddr);

  struct ip_mreq mreq;
  mreq.imr_multiaddr.s_addr = inet_addr(multicastAddr);         
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);

  if(setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) == -1) {
      fprintf(stderr, "setsockopt: %d\n", errno);
      return -1;
  }

  if (bind(sock, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
      fprintf(stderr, "bind: %d\n", errno);
      return -1;
  }
  return sock;
}

char *lw_mc_addr(int channelNumber)
{
  struct in_addr addr;
  addr.s_addr = htonl(0xEFC00000 + channelNumber);
  char *s = inet_ntoa(addr);
  return s;
}

INT_PCM pcm24_to_pcm16(int32_t PCM24bit)
{
  if (PCM24bit & 0x800000)
    PCM24bit = PCM24bit | ~0xFFFFFF;
  INT_PCM PCM16bit = PCM24bit >> 8;
  return PCM16bit; // 16-bit signed PCM
}

int main(int argc, const char *argv[]) {
  if (argc != 4) {
    printf("Usage: axiadump <LW channel> <file.out> <Duration sec.>\n");
    return 1;
  }

  uint16_t axiaChannel = atoi(argv[1]);
  const char *outputFile = argv[2];
  uint32_t duration = 48000 * atof(argv[3]);

  HANDLE_AACENCODER encoder_handle = NULL;
  if (aacEncOpen(&encoder_handle, 0, 2) != AACENC_OK) {
    fprintf(stderr, "Unable to open encoder\n");
    return 1;
  }
  if (aacEncoder_SetParam(encoder_handle, AACENC_AOT, 2) != AACENC_OK) {
    fprintf(stderr, "Unable to set the AOT\n");
    return 1;
  }
  if (aacEncoder_SetParam(encoder_handle, AACENC_SAMPLERATE, 48000) != AACENC_OK) {
    fprintf(stderr, "Unable to set the sample rate\n");
    return 1;
  }
  if (aacEncoder_SetParam(encoder_handle, AACENC_CHANNELMODE, MODE_2) != AACENC_OK) {
    fprintf(stderr, "Unable to set the channel mode\n");
    return 1;
  }
  if (aacEncoder_SetParam(encoder_handle, AACENC_CHANNELORDER, 1) != AACENC_OK) {
    fprintf(stderr, "Unable to set the wav channel order\n");
    return 1;
  }
  if (aacEncoder_SetParam(encoder_handle, AACENC_BITRATE, 256000) != AACENC_OK) {
      fprintf(stderr, "Unable to set the bitrate\n");
      return 1;
    }
  if (aacEncoder_SetParam(encoder_handle, AACENC_TRANSMUX, 2) != AACENC_OK) {
    fprintf(stderr, "Unable to set the ADTS transmux\n");
    return 1;
  }
  if (aacEncoder_SetParam(encoder_handle, AACENC_AFTERBURNER, 1) != AACENC_OK) {
    fprintf(stderr, "Unable to set the afterburner mode\n");
    return 1;
  }
  if (aacEncEncode(encoder_handle, NULL, NULL, NULL, NULL) != AACENC_OK) {
    fprintf(stderr, "Unable to initialize the encoder\n");
    return 1;
  }

  AACENC_InfoStruct info = { 0 };
  if (aacEncInfo(encoder_handle, &info) != AACENC_OK) {
    fprintf(stderr, "Unable to get the encoder info\n");
    return 1;
  }  
  printf("%i\n", info.frameLength); //<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

  int sock = config_livewire_socket(lw_mc_addr(axiaChannel));
  if (sock == -1) {
    fprintf(stderr, "Error opening Livewire socket.");
    return 1;
  }

  FILE *file = fopen(outputFile, "wb");
  if (file == NULL) {
    fprintf(stderr, "Error opening file: %d\n", errno);
    return 1;
  }

  printf("Writing Livewire source #%i to %s\nDuration: %i sec.\n", axiaChannel, outputFile, duration / 48000);
  uint32_t frameCounter = 0;

  while (1) {
    uint8_t packet[1452];
    INT_PCM PCM_16_signed[480];
    int packetLength = recv(sock, packet, sizeof(packet), 0);
    //uint16_t sequenceNumber = (packet[2] << 8 | packet[3]);
    frameCounter += ((packetLength - 12) / 6);

    for (int i = 12; i < packetLength; i += 3) {
      int32_t PCM_24_unsigned = ((packet[i] << 16) | (packet[i + 1] << 8) | (packet[i + 2]));
      PCM_16_signed[((i - 12) / 3)] = pcm24_to_pcm16(PCM_24_unsigned);
      printf("%i %i\n", ((i - 12) / 3), PCM_16_signed[((i - 12) / 3)]);
    }

    AACENC_BufDesc in_buf = { 0 }, out_buf = { 0 };
    AACENC_InArgs in_args = { 0 };
    AACENC_OutArgs out_args = { 0 };
    int in_identifier = IN_AUDIO_DATA;
    int in_size, in_elem_size;
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size, out_elem_size;
    void *in_ptr, *out_ptr;
    uint8_t outbuf[1536];
    AACENC_ERROR err;

    if (frameCounter >= duration) {
      in_args.numInSamples = -1;
    } else {
      in_ptr = PCM_16_signed;
      in_size = (((packetLength - 12) / 3) * sizeof(INT_PCM));
      in_elem_size = sizeof(INT_PCM);
      in_identifier = IN_AUDIO_DATA;
      in_args.numInSamples = ((packetLength - 12) / 3); 
      in_buf.numBufs = 1;
      in_buf.bufs = &in_ptr;
      in_buf.bufferIdentifiers = &in_identifier;
      in_buf.bufSizes = &in_size;
      in_buf.bufElSizes = &in_elem_size;
    }

    out_ptr = outbuf;
    out_size = 1536;
    out_elem_size = sizeof(uint8_t);
    out_identifier = OUT_BITSTREAM_DATA;
    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;

    //printf("%d\n", in_size); //<<<<<<<<<<<<<<<<<<<<<<<<

    if ((err = aacEncEncode(encoder_handle, &in_buf, &out_buf, &in_args, &out_args)) != AACENC_OK) {
      if (err == AACENC_ENCODE_EOF)
        break;
      fprintf(stderr, "Encoding failed.\n");
      return 1;
    }
    if (out_args.numOutBytes == 0)
      continue;
    fwrite(outbuf, 1, out_args.numOutBytes, file);
  }

  aacEncClose(&encoder_handle);
  close(sock);
  fclose(file);
  printf("Done!\n");
  return 0;
}
