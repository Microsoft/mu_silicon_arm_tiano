/** @file

Copyright (c) 2006, Intel Corporation
All rights reserved. This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

Module Name:

  Mtftp4Option.c

Abstract:
  routines to process MTFTP4 options


**/

#include "Mtftp4Impl.h"

CHAR8 *mMtftp4SupportedOptions[MTFTP4_SUPPORTED_OPTIONS] = {
  "blksize",
  "timeout",
  "tsize",
  "multicast"
};


/**
  Go through the packet to fill the Options array with the start
  addresses of each MTFTP option name/value pair.

  @param  Packet                 The packet to check
  @param  PacketLen              The packet's length
  @param  Count                  The size of the Options on input. The actual
                                 options on output
  @param  Options                The option array to fill in

  @retval EFI_INVALID_PARAMETER  The packet is mal-formated
  @retval EFI_BUFFER_TOO_SMALL   The Options array is too small
  @retval EFI_SUCCESS            The packet has been parsed into the Options array.

**/
STATIC
EFI_STATUS
Mtftp4FillOptions (
  IN  EFI_MTFTP4_PACKET     *Packet,
  IN  UINT32                PacketLen,
  IN  OUT UINT32            *Count,
  OUT EFI_MTFTP4_OPTION     *Options          OPTIONAL
  )
{
  UINT8                     *Cur;
  UINT8                     *Last;
  UINT8                     Num;
  UINT8                     *Name;
  UINT8                     *Value;

  Num   = 0;
  Cur   = (UINT8 *) Packet + MTFTP4_OPCODE_LEN;
  Last  = (UINT8 *) Packet + PacketLen - 1;

  //
  // process option name and value pairs. The last byte is always zero
  //
  while (Cur < Last) {
    Name = Cur;

    while (*Cur != 0) {
      Cur++;
    }

    if (Cur == Last) {
      return EFI_INVALID_PARAMETER;
    }

    Value = ++Cur;

    while (*Cur != 0) {
      Cur++;
    }

    Num++;

    if ((Options != NULL) && (Num <= *Count)) {
      Options[Num - 1].OptionStr  = Name;
      Options[Num - 1].ValueStr   = Value;
    }

    Cur++;
  }

  if ((*Count < Num) || (Options == NULL)) {
    *Count = Num;
    return EFI_BUFFER_TOO_SMALL;
  }

  *Count = Num;
  return EFI_SUCCESS;
}


/**
  Allocate and fill in a array of Mtftp options from the Packet. It
  first calls Mtftp4FillOption to get the option number, then allocate
  the array, at last, call Mtftp4FillOption again to save the options.

  @param  Packet                 The packet to parse
  @param  PacketLen              The length of the packet
  @param  OptionCount            The number of options in the packet
  @param  OptionList             The point to get the option array.

  @retval EFI_INVALID_PARAMETER  The parametera are invalid or packet isn't a
                                 well-formated OACK packet.
  @retval EFI_SUCCESS            The option array is build
  @retval EFI_OUT_OF_RESOURCES   Failed to allocate memory for the array

**/
EFI_STATUS
Mtftp4ExtractOptions (
  IN  EFI_MTFTP4_PACKET     *Packet,
  IN  UINT32                PacketLen,
  IN  OUT UINT32            *OptionCount,
  OUT EFI_MTFTP4_OPTION     **OptionList        OPTIONAL
  )
{
  EFI_STATUS                Status;

  *OptionCount = 0;

  if (OptionList != NULL) {
    *OptionList = NULL;
  }

  if (NTOHS (Packet->OpCode) != EFI_MTFTP4_OPCODE_OACK) {
    return EFI_INVALID_PARAMETER;
  }

  if (PacketLen == MTFTP4_OPCODE_LEN) {
    return EFI_SUCCESS;
  }

  //
  // The last byte must be zero to terminate the options
  //
  if (*((UINT8 *) Packet + PacketLen - 1) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get the number of options
  //
  Status = Mtftp4FillOptions (Packet, PacketLen, OptionCount, NULL);

  if ((Status == EFI_SUCCESS) || (Status != EFI_BUFFER_TOO_SMALL)) {
    return Status;
  }

  //
  // Allocate memory for the options, then call Mtftp4FillOptions to
  // fill it if caller want that.
  //
  if (OptionList == NULL) {
    return EFI_SUCCESS;
  }

  *OptionList = AllocatePool (*OptionCount * sizeof (EFI_MTFTP4_OPTION));

  if (*OptionList == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Mtftp4FillOptions (Packet, PacketLen, OptionCount, *OptionList);
  return EFI_SUCCESS;
}


/**
  Check whether two ascii strings are equel, ignore the case.

  @param  Str1                   The first ascii string
  @param  Str2                   The second ascii string

  @retval TRUE                   Two strings are equal when case is ignored.
  @retval FALSE                  Two string are not equal.

**/
BOOLEAN
NetStringEqualNoCase (
  IN  UINT8                 *Str1,
  IN  UINT8                 *Str2
  )
{
  UINT8                     Ch1;
  UINT8                     Ch2;

  ASSERT ((Str1 != NULL) && (Str2 != NULL));

  for (; (*Str1 != '\0') && (*Str2 != '\0'); Str1++, Str2++) {
    Ch1 = *Str1;
    Ch2 = *Str2;

    //
    // Convert them to lower case then compare two
    //
    if (('A' <= Ch1) && (Ch1 <= 'Z')) {
      Ch1 += 'a' - 'A';
    }

    if (('A' <= Ch2) && (Ch2 <= 'Z')) {
      Ch2 += 'a' - 'A';
    }

    if (Ch1 != Ch2) {
      return FALSE;
    }
  }

  return (BOOLEAN) (*Str1 == *Str2);
}


/**
  Convert a string to a UINT32 number.

  @param  Str                    The string to convert from

  @return The number get from the string

**/
UINT32
NetStringToU32 (
  IN  UINT8                 *Str
  )
{
  UINT32                    Num;

  ASSERT (Str != NULL);

  Num = 0;

  for (; NET_IS_DIGIT (*Str); Str++) {
    Num = Num * 10 + (*Str - '0');
  }

  return Num;
}


/**
  Convert a string of the format "192.168.0.1" to an IP address.

  @param  Str                    The string representation of IP
  @param  Ip                     The varible to get IP.

  @retval EFI_INVALID_PARAMETER  The IP string is invalid.
  @retval EFI_SUCCESS            The IP is parsed into the Ip

**/
STATIC
EFI_STATUS
NetStringToIp (
  IN  UINT8                 *Str,
  OUT IP4_ADDR              *Ip
  )
{
  UINT32                    Byte;
  UINT32                    Addr;
  UINTN                     Index;

  *Ip  = 0;
  Addr = 0;

  for (Index = 0; Index < 4; Index++) {
    if (!NET_IS_DIGIT (*Str)) {
      return EFI_INVALID_PARAMETER;
    }

    Byte = NetStringToU32 (Str);

    if (Byte > 255) {
      return EFI_INVALID_PARAMETER;
    }

    Addr = (Addr << 8) | Byte;

    //
    // Skip all the digitals and check whether the sepeator is the dot
    //
    while (NET_IS_DIGIT (*Str)) {
      Str++;
    }

    if ((Index < 3) && (*Str != '.')) {
      return EFI_INVALID_PARAMETER;
    }

    Str++;
  }

  *Ip = Addr;

  return EFI_SUCCESS;
}


/**
  Parse the MTFTP multicast option.

  @param  Value                  The Mtftp multicast value string
  @param  Option                 The option to save the info into.

  @retval EFI_INVALID_PARAMETER  The multicast value string is invalid.
  @retval EFI_SUCCESS            The multicast value is parsed into the Option

**/
STATIC
EFI_STATUS
Mtftp4ExtractMcast (
  IN UINT8                  *Value,
  IN MTFTP4_OPTION          *Option
  )
{
  EFI_STATUS                Status;
  UINT32                    Num;

  //
  // The multicast option is formated like "204.0.0.1,1857,1"
  // The server can also omit the ip and port, use ",,1"
  //
  if (*Value == ',') {
    Option->McastIp   = 0;
  } else {
    Status = NetStringToIp (Value, &Option->McastIp);

    if (EFI_ERROR (Status)) {
      return Status;
    }

    while (*Value && (*Value != ',')) {
      Value++;
    }
  }

  if (*Value != ',') {
    return EFI_INVALID_PARAMETER;
  }

  Value++;

  //
  // Convert the port setting. the server can send us a port number or
  // empty string. such as the port in ",,1"
  //
  if (*Value == ',') {
    Option->McastPort = 0;
  } else {
    Num = NetStringToU32 (Value);

    if (Num > 65535) {
      return EFI_INVALID_PARAMETER;
    }

    Option->McastPort = (UINT16)Num;

    while (NET_IS_DIGIT (*Value)) {
      Value++;
    }
  }

  if (*Value != ',') {
    return EFI_INVALID_PARAMETER;
  }

  Value++;

  //
  // Check the master/slave setting, 1 for master, 0 for slave.
  //
  Num = NetStringToU32 (Value);

  if ((Num != 0) && (Num != 1)) {
    return EFI_INVALID_PARAMETER;
  }

  Option->Master = (BOOLEAN)(Num == 1);

  while (NET_IS_DIGIT (*Value)) {
    Value++;
  }

  if (*Value != '\0') {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}


/**
  Parse the option in Options array to MTFTP4_OPTION which program
  can access directly.

  @param  Options                The option array, which contains addresses of each
                                  option's name/value string.
  @param  Count                  The number of options in the Options
  @param  Request                Whether this is a request or OACK. The format of
                                 multicast is different according to this setting.
  @param  MtftpOption            The MTFTP4_OPTION for easy access.

  @retval EFI_INVALID_PARAMETER  The option is mal-formated
  @retval EFI_UNSUPPORTED        Some option isn't supported
  @retval EFI_SUCCESS            The option are OK and has been parsed.

**/
EFI_STATUS
Mtftp4ParseOption (
  IN  EFI_MTFTP4_OPTION     *Options,
  IN  UINT32                Count,
  IN  BOOLEAN               Request,
  OUT MTFTP4_OPTION         *MtftpOption
  )
{
  EFI_STATUS                Status;
  UINT32                    Index;
  UINT32                    Value;
  EFI_MTFTP4_OPTION         *This;

  MtftpOption->Exist = 0;

  for (Index = 0; Index < Count; Index++) {
    This = Options + Index;

    if ((This->OptionStr == NULL) || (This->ValueStr == NULL)) {
      return EFI_INVALID_PARAMETER;
    }

    if (NetStringEqualNoCase (This->OptionStr, (UINT8 *) "blksize")) {
      //
      // block size option, valid value is between [8, 65464]
      //
      Value = NetStringToU32 (This->ValueStr);

      if ((Value < 8) || (Value > 65464)) {
        return EFI_INVALID_PARAMETER;
      }

      MtftpOption->BlkSize = (UINT16) Value;
      MtftpOption->Exist |= MTFTP4_BLKSIZE_EXIST;

    } else if (NetStringEqualNoCase (This->OptionStr, (UINT8 *) "timeout")) {
      //
      // timeout option, valid value is between [1, 255]
      //
      Value = NetStringToU32 (This->ValueStr);

      if ((Value < 1) || (Value > 255)) {
        return EFI_INVALID_PARAMETER;
      }

      MtftpOption->Timeout = (UINT8) Value;

    } else if (NetStringEqualNoCase (This->OptionStr, (UINT8 *) "tsize")) {
      //
      // tsize option, the biggest transfer supported is 4GB with block size option
      //
      MtftpOption->Tsize = NetStringToU32 (This->ValueStr);
      MtftpOption->Exist |= MTFTP4_TSIZE_EXIST;

    } else if (NetStringEqualNoCase (This->OptionStr, (UINT8 *) "multicast")) {
      //
      // Multicast option, if it is a request, the value must be a zero
      // length string, otherwise, it is formated like "204.0.0.1,1857,1\0"
      //
      if (Request) {
        if (*(This->ValueStr) != '\0') {
          return EFI_INVALID_PARAMETER;
        }

      } else {
        Status = Mtftp4ExtractMcast (This->ValueStr, MtftpOption);

        if (EFI_ERROR (Status)) {
          return Status;
        }
      }

      MtftpOption->Exist |= MTFTP4_MCAST_EXIST;

    } else if (Request) {
      //
      // Ignore the unsupported option if it is a reply, and return
      // EFI_UNSUPPORTED if it's a request according to the UEFI spec.
      //
      return EFI_UNSUPPORTED;
    }
  }

  return EFI_SUCCESS;
}


/**
  Parse the options in the OACK packet to MTFTP4_OPTION which program
  can access directly.

  @param  Packet                 The OACK packet to parse
  @param  PacketLen              The length of the packet
  @param  MtftpOption            The MTFTP_OPTION for easy access.

  @retval EFI_INVALID_PARAMETER  The packet option is mal-formated
  @retval EFI_UNSUPPORTED        Some option isn't supported
  @retval EFI_SUCCESS            The option are OK and has been parsed.

**/
EFI_STATUS
Mtftp4ParseOptionOack (
  IN  EFI_MTFTP4_PACKET     *Packet,
  IN  UINT32                PacketLen,
  OUT MTFTP4_OPTION         *MtftpOption
  )
{
  EFI_MTFTP4_OPTION *OptionList;
  EFI_STATUS        Status;
  UINT32            Count;

  MtftpOption->Exist = 0;

  Status = Mtftp4ExtractOptions (Packet, PacketLen, &Count, &OptionList);

  if (EFI_ERROR (Status) || (Count == 0)) {
    return Status;
  }

  Status = Mtftp4ParseOption (OptionList, Count, FALSE, MtftpOption);

  gBS->FreePool (OptionList);
  return Status;
}
