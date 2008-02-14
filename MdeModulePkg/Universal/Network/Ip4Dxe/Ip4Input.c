/** @file

Copyright (c) 2005 - 2007, Intel Corporation
All rights reserved. This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

Module Name:

  Ip4Input.c

Abstract:

  IP4 input process.


**/

#include "Ip4Impl.h"


/**
  Create a empty assemble entry for the packet identified by
  (Dst, Src, Id, Protocol). The default life for the packet is
  120 seconds.

  @param  Dst                    The destination address
  @param  Src                    The source address
  @param  Id                     The ID field in IP header
  @param  Protocol               The protocol field in IP header

  @return NULL if failed to allocate memory for the entry, otherwise
  @return the point to just created reassemble entry.

**/
STATIC
IP4_ASSEMBLE_ENTRY *
Ip4CreateAssembleEntry (
  IN IP4_ADDR               Dst,
  IN IP4_ADDR               Src,
  IN UINT16                 Id,
  IN UINT8                  Protocol
  )
{

  IP4_ASSEMBLE_ENTRY        *Assemble;

  Assemble = AllocatePool (sizeof (IP4_ASSEMBLE_ENTRY));

  if (Assemble == NULL) {
    return NULL;
  }

  InitializeListHead (&Assemble->Link);
  InitializeListHead (&Assemble->Fragments);

  Assemble->Dst      = Dst;
  Assemble->Src      = Src;
  Assemble->Id       = Id;
  Assemble->Protocol = Protocol;
  Assemble->TotalLen = 0;
  Assemble->CurLen   = 0;
  Assemble->Head     = NULL;
  Assemble->Info     = NULL;
  Assemble->Life     = IP4_FRAGMENT_LIFE;

  return Assemble;
}


/**
  Release all the fragments of a packet, then free the assemble entry

  @param  Assemble               The assemble entry to free

  @return None

**/
STATIC
VOID
Ip4FreeAssembleEntry (
  IN IP4_ASSEMBLE_ENTRY     *Assemble
  )
{
  LIST_ENTRY                *Entry;
  LIST_ENTRY                *Next;
  NET_BUF                   *Fragment;

  NET_LIST_FOR_EACH_SAFE (Entry, Next, &Assemble->Fragments) {
    Fragment = NET_LIST_USER_STRUCT (Entry, NET_BUF, List);

    RemoveEntryList (Entry);
    NetbufFree (Fragment);
  }

  gBS->FreePool (Assemble);
}


/**
  Initialize an already allocated assemble table. This is generally
  the assemble table embedded in the IP4 service instance.

  @param  Table                  The assemble table to initialize.

  @return NONE

**/
VOID
Ip4InitAssembleTable (
  IN IP4_ASSEMBLE_TABLE     *Table
  )
{
  UINT32                    Index;

  for (Index = 0; Index < IP4_ASSEMLE_HASH_SIZE; Index++) {
    InitializeListHead (&Table->Bucket[Index]);
  }
}


/**
  Clean up the assemble table: remove all the fragments
  and assemble entries.

  @param  Table                  The assemble table to clean up

  @return None

**/
VOID
Ip4CleanAssembleTable (
  IN IP4_ASSEMBLE_TABLE     *Table
  )
{
  LIST_ENTRY                *Entry;
  LIST_ENTRY                *Next;
  IP4_ASSEMBLE_ENTRY        *Assemble;
  UINT32                    Index;

  for (Index = 0; Index < IP4_ASSEMLE_HASH_SIZE; Index++) {
    NET_LIST_FOR_EACH_SAFE (Entry, Next, &Table->Bucket[Index]) {
      Assemble = NET_LIST_USER_STRUCT (Entry, IP4_ASSEMBLE_ENTRY, Link);

      RemoveEntryList (Entry);
      Ip4FreeAssembleEntry (Assemble);
    }
  }
}


/**
  Trim the packet to fit in [Start, End), and update the per
  packet information.

  @param  Packet                 Packet to trim
  @param  Start                  The sequence of the first byte to fit in
  @param  End                    One beyond the sequence of last byte to fit in.

  @return None

**/
STATIC
VOID
Ip4TrimPacket (
  IN NET_BUF                *Packet,
  IN INTN                   Start,
  IN INTN                   End
  )
{
  IP4_CLIP_INFO             *Info;
  INTN                      Len;

  Info = IP4_GET_CLIP_INFO (Packet);

  ASSERT (Info->Start + Info->Length == Info->End);
  ASSERT ((Info->Start < End) && (Start < Info->End));

   if (Info->Start < Start) {
    Len = Start - Info->Start;

    NetbufTrim (Packet, (UINT32) Len, NET_BUF_HEAD);
    Info->Start   = Start;
    Info->Length -= Len;
  }

  if (End < Info->End) {
    Len = End - Info->End;

    NetbufTrim (Packet, (UINT32) Len, NET_BUF_TAIL);
    Info->End     = End;
    Info->Length -= Len;
  }
}


/**
  Release all the fragments of the packet. This is the callback for
  the assembled packet's OnFree. It will free the assemble entry,
  which in turn will free all the fragments of the packet.

  @param  Arg                    The assemble entry to free

  @return None

**/
STATIC
VOID
Ip4OnFreeFragments (
  IN VOID                   *Arg
  )
{
  Ip4FreeAssembleEntry ((IP4_ASSEMBLE_ENTRY *) Arg);
}


/**
  Reassemble the IP fragments. If all the fragments of the packet
  have been received, it will wrap the packet in a net buffer then
  return it to caller. If the packet can't be assembled, NULL is
  return.

  @param  Table                  The assemble table used.
  @param  Packet                 The fragment to assemble

  @return NULL if the packet can't be reassemble. The point to just assembled
  @return packet if all the fragments of the packet have arrived.

**/
STATIC
NET_BUF *
Ip4Reassemble (
  IN IP4_ASSEMBLE_TABLE     *Table,
  IN NET_BUF                *Packet
  )
{
  IP4_HEAD                  *IpHead;
  IP4_CLIP_INFO             *This;
  IP4_CLIP_INFO             *Node;
  IP4_ASSEMBLE_ENTRY        *Assemble;
  LIST_ENTRY                *Head;
  LIST_ENTRY                *Prev;
  LIST_ENTRY                *Cur;
  NET_BUF                   *Fragment;
  NET_BUF                   *NewPacket;
  INTN                      Index;

  IpHead  = Packet->Ip;
  This    = IP4_GET_CLIP_INFO (Packet);

  ASSERT (IpHead != NULL);

  //
  // First: find the related assemble entry
  //
  Assemble  = NULL;
  Index     = IP4_ASSEMBLE_HASH (IpHead->Dst, IpHead->Src, IpHead->Id, IpHead->Protocol);

  NET_LIST_FOR_EACH (Cur, &Table->Bucket[Index]) {
    Assemble = NET_LIST_USER_STRUCT (Cur, IP4_ASSEMBLE_ENTRY, Link);

    if ((Assemble->Dst == IpHead->Dst) && (Assemble->Src == IpHead->Src) &&
        (Assemble->Id == IpHead->Id)   && (Assemble->Protocol == IpHead->Protocol)) {
      break;
    }
  }

  //
  // Create a new assemble entry if no assemble entry is related to this packet
  //
  if (Cur == &Table->Bucket[Index]) {
    Assemble = Ip4CreateAssembleEntry (
                 IpHead->Dst,
                 IpHead->Src,
                 IpHead->Id,
                 IpHead->Protocol
                 );

    if (Assemble == NULL) {
      goto DROP;
    }

    InsertHeadList (&Table->Bucket[Index], &Assemble->Link);
  }

  //
  // Find the point to insert the packet: before the first
  // fragment with THIS.Start < CUR.Start. the previous one
  // has PREV.Start <= THIS.Start < CUR.Start.
  //
  Head = &Assemble->Fragments;

  NET_LIST_FOR_EACH (Cur, Head) {
    Fragment = NET_LIST_USER_STRUCT (Cur, NET_BUF, List);

    if (This->Start < IP4_GET_CLIP_INFO (Fragment)->Start) {
      break;
    }
  }

  //
  // Check whether the current fragment overlaps with the previous one.
  // It holds that: PREV.Start <= THIS.Start < THIS.End. Only need to
  // check whether THIS.Start < PREV.End for overlap. If two fragments
  // overlaps, trim the overlapped part off THIS fragment.
  //
  if ((Prev = Cur->ForwardLink) != Head) {
    Fragment  = NET_LIST_USER_STRUCT (Prev, NET_BUF, List);
    Node      = IP4_GET_CLIP_INFO (Fragment);

    if (This->Start < Node->End) {
      if (This->End <= Node->End) {
        NetbufFree (Packet);
        return NULL;
      }

      Ip4TrimPacket (Packet, Node->End, This->End);
    }
  }

  //
  // Insert the fragment into the packet. The fragment may be removed
  // from the list by the following checks.
  //
  NetListInsertBefore (Cur, &Packet->List);

  //
  // Check the packets after the insert point. It holds that:
  // THIS.Start <= NODE.Start < NODE.End. The equality holds
  // if PREV and NEXT are continuous. THIS fragment may fill
  // several holes. Remove the completely overlapped fragments
  //
  while (Cur != Head) {
    Fragment = NET_LIST_USER_STRUCT (Cur, NET_BUF, List);
    Node     = IP4_GET_CLIP_INFO (Fragment);

    //
    // Remove fragments completely overlapped by this fragment
    //
    if (Node->End <= This->End) {
      Cur = Cur->ForwardLink;

      RemoveEntryList (&Fragment->List);
      Assemble->CurLen -= Node->Length;

      NetbufFree (Fragment);
      continue;
    }

    //
    // The conditions are: THIS.Start <= NODE.Start, and THIS.End <
    // NODE.End. Two fragments overlaps if NODE.Start < THIS.End.
    // If two fragments start at the same offset, remove THIS fragment
    // because ((THIS.Start == NODE.Start) && (THIS.End < NODE.End)).
    //
    if (Node->Start < This->End) {
      if (This->Start == Node->Start) {
        RemoveEntryList (&Packet->List);
        goto DROP;
      }

      Ip4TrimPacket (Packet, This->Start, Node->Start);
    }

    break;
  }

  //
  // Update the assemble info: increase the current length. If it is
  // the frist fragment, update the packet's IP head and per packet
  // info. If it is the last fragment, update the total length.
  //
  Assemble->CurLen += This->Length;

  if (This->Start == 0) {
    //
    // Once the first fragment is enqueued, it can't be removed
    // from the fragment list. So, Assemble->Head always point
    // to valid memory area.
    //
    ASSERT (Assemble->Head == NULL);

    Assemble->Head  = IpHead;
    Assemble->Info  = IP4_GET_CLIP_INFO (Packet);
  }

  //
  // Don't update the length more than once.
  //
  if (IP4_LAST_FRAGMENT (IpHead->Fragment) && (Assemble->TotalLen == 0)) {
    Assemble->TotalLen = This->End;
  }

  //
  // Deliver the whole packet if all the fragments received.
  // All fragments received if:
  //  1. received the last one, so, the totoal length is know
  //  2. received all the data. If the last fragment on the
  //     queue ends at the total length, all data is received.
  //
  if ((Assemble->TotalLen != 0) && (Assemble->CurLen >= Assemble->TotalLen)) {

    RemoveEntryList (&Assemble->Link);

    //
    // If the packet is properly formated, the last fragment's End
    // equals to the packet's total length. Otherwise, the packet
    // is a fake, drop it now.
    //
    Fragment = NET_LIST_USER_STRUCT (Head->BackLink, NET_BUF, List);

    if (IP4_GET_CLIP_INFO (Fragment)->End != Assemble->TotalLen) {
      Ip4FreeAssembleEntry (Assemble);
      return NULL;
    }

    //
    // Wrap the packet in a net buffer then deliver it up
    //
    NewPacket = NetbufFromBufList (
                  &Assemble->Fragments,
                  0,
                  0,
                  Ip4OnFreeFragments,
                  Assemble
                  );

    if (NewPacket == NULL) {
      Ip4FreeAssembleEntry (Assemble);
      return NULL;
    }

    NewPacket->Ip                  = Assemble->Head;
    CopyMem (IP4_GET_CLIP_INFO (NewPacket), Assemble->Info, sizeof (*IP4_GET_CLIP_INFO (NewPacket)));
    return NewPacket;
  }

  return NULL;

DROP:
  NetbufFree (Packet);
  return NULL;
}


/**
  The IP4 input routine. It is called by the IP4_INTERFACE when a
  IP4 fragment is received from MNP.

  @param  Ip4Instance            The IP4 child that request the receive, most like
                                 it is NULL.
  @param  Packet                 The IP4 packet received.
  @param  IoStatus               The return status of receive request.
  @param  Flag                   The link layer flag for the packet received, such
                                 as multicast.
  @param  Context                The IP4 service instance that own the MNP.

  @return None

**/
VOID
Ip4AccpetFrame (
  IN IP4_PROTOCOL           *Ip4Instance,
  IN NET_BUF                *Packet,
  IN EFI_STATUS             IoStatus,
  IN UINT32                 Flag,
  IN VOID                   *Context
  )
{
  IP4_SERVICE               *IpSb;
  IP4_CLIP_INFO             *Info;
  IP4_HEAD                  *Head;
  UINT32                    HeadLen;
  UINT32                    OptionLen;
  UINT32                    TotalLen;
  UINT16                    Checksum;

  IpSb = (IP4_SERVICE *) Context;

  if (EFI_ERROR (IoStatus) || (IpSb->State == IP4_SERVICE_DESTORY)) {
    goto DROP;
  }

  //
  // Check that the IP4 header is correctly formated
  //
  if (Packet->TotalSize < IP4_MIN_HEADLEN) {
    goto RESTART;
  }

  Head     = (IP4_HEAD *) NetbufGetByte (Packet, 0, NULL);
  HeadLen  = (Head->HeadLen << 2);
  TotalLen = NTOHS (Head->TotalLen);

  //
  // Mnp may deliver frame trailer sequence up, trim it off.
  //
  if (TotalLen < Packet->TotalSize) {
    NetbufTrim (Packet, Packet->TotalSize - TotalLen, FALSE);
  }

  if ((Head->Ver != 4) || (HeadLen < IP4_MIN_HEADLEN) ||
      (TotalLen < HeadLen) || (TotalLen != Packet->TotalSize)) {
    goto RESTART;
  }

  //
  // Some OS may send IP packets without checksum.
  //
  Checksum = (UINT16) (~NetblockChecksum ((UINT8 *) Head, HeadLen));

  if ((Head->Checksum != 0) && (Checksum != 0)) {
    goto RESTART;
  }

  //
  // Convert the IP header to host byte order, then get the per packet info.
  //
  Packet->Ip      = Ip4NtohHead (Head);

  Info            = IP4_GET_CLIP_INFO (Packet);
  Info->LinkFlag  = Flag;
  Info->CastType  = Ip4GetHostCast (IpSb, Head->Dst, Head->Src);
  Info->Start     = (Head->Fragment & IP4_HEAD_OFFSET_MASK) << 3;
  Info->Length    = Head->TotalLen - HeadLen;
  Info->End       = Info->Start + Info->Length;
  Info->Status    = EFI_SUCCESS;

  //
  // The packet is destinated to us if the CastType is non-zero.
  //
  if ((Info->CastType == 0) || (Info->End > IP4_MAX_PACKET_SIZE)) {
    goto RESTART;
  }

  //
  // Validate the options. Don't call the Ip4OptionIsValid if
  // there is no option to save some CPU process.
  //
  OptionLen = HeadLen - IP4_MIN_HEADLEN;

  if ((OptionLen > 0) && !Ip4OptionIsValid ((UINT8 *) (Head + 1), OptionLen, TRUE)) {
    goto RESTART;
  }

  //
  // Trim the head off, after this point, the packet is headless.
  // and Packet->TotalLen == Info->Length.
  //
  NetbufTrim (Packet, HeadLen, TRUE);

  //
  // Reassemble the packet if this is a fragment. The packet is a
  // fragment if its head has MF (more fragment) set, or it starts
  // at non-zero byte.
  //
  if ((Head->Fragment & IP4_HEAD_MF_MASK) || (Info->Start != 0)) {
    //
    // Drop the fragment if DF is set but it is fragmented. Gateway
    // need to send a type 4 destination unreache ICMP message here.
    //
    if (Head->Fragment & IP4_HEAD_DF_MASK) {
      goto RESTART;
    }

    //
    // The length of all but the last fragments is in the unit of 8 bytes.
    //
    if ((Head->Fragment & IP4_HEAD_MF_MASK) && (Info->Length % 8 != 0)) {
      goto RESTART;
    }

    Packet = Ip4Reassemble (&IpSb->Assemble, Packet);

    //
    // Packet assembly isn't complete, start receive more packet.
    //
    if (Packet == NULL) {
      goto RESTART;
    }
  }

  //
  // Packet may have been changed. Head, HeadLen, TotalLen, and
  // info must be reloaded bofore use. The ownership of the packet
  // is transfered to the packet process logic.
  //
  Head  = Packet->Ip;
  IP4_GET_CLIP_INFO (Packet)->Status = EFI_SUCCESS;

  switch (Head->Protocol) {
  case IP4_PROTO_ICMP:
    Ip4IcmpHandle (IpSb, Head, Packet);
    break;

  case IP4_PROTO_IGMP:
    Ip4IgmpHandle (IpSb, Head, Packet);
    break;

  default:
    Ip4Demultiplex (IpSb, Head, Packet);
  }

  Packet = NULL;

  //
  // Dispatch the DPCs queued by the NotifyFunction of the rx token's events
  // which are signaled with received data.
  //
  NetLibDispatchDpc ();

RESTART:
  Ip4ReceiveFrame (IpSb->DefaultInterface, NULL, Ip4AccpetFrame, IpSb);

DROP:
  if (Packet != NULL) {
    NetbufFree (Packet);
  }

  return ;
}


/**
  Check whether this IP child accepts the packet.

  @param  IpInstance             The IP child to check
  @param  Head                   The IP header of the packet
  @param  Packet                 The data of the packet

  @return TRUE if the child wants to receive the packet, otherwise return FALSE.

**/
BOOLEAN
Ip4InstanceFrameAcceptable (
  IN IP4_PROTOCOL           *IpInstance,
  IN IP4_HEAD               *Head,
  IN NET_BUF                *Packet
  )
{
  IP4_ICMP_ERROR_HEAD       Icmp;
  EFI_IP4_CONFIG_DATA       *Config;
  IP4_CLIP_INFO             *Info;
  UINT16                    Proto;
  UINT32                    Index;

  Config = &IpInstance->ConfigData;

  //
  // Dirty trick for the Tiano UEFI network stack implmentation. If
  // ReceiveTimeout == -1, the receive of the packet for this instance
  // is disabled. The UEFI spec don't have such captibility. We add
  // this to improve the performance because IP will make a copy of
  // the received packet for each accepting instance. Some IP instances
  // used by UDP/TCP only send packets, they don't wants to receive.
  //
  if (Config->ReceiveTimeout == (UINT32)(-1)) {
    return FALSE;
  }

  if (Config->AcceptPromiscuous) {
    return TRUE;
  }

  //
  // Use protocol from the IP header embedded in the ICMP error
  // message to filter, instead of ICMP itself. ICMP handle will
  // can Ip4Demultiplex to deliver ICMP errors.
  //
  Proto = Head->Protocol;

  if (Proto == IP4_PROTO_ICMP) {
    NetbufCopy (Packet, 0, sizeof (Icmp.Head), (UINT8 *) &Icmp.Head);

    if (mIcmpClass[Icmp.Head.Type].IcmpClass == ICMP_ERROR_MESSAGE) {
      if (!Config->AcceptIcmpErrors) {
        return FALSE;
      }

      NetbufCopy (Packet, 0, sizeof (Icmp), (UINT8 *) &Icmp);
      Proto = Icmp.IpHead.Protocol;
    }
  }

  //
  // Match the protocol
  //
  if (!Config->AcceptAnyProtocol && (Proto != Config->DefaultProtocol)) {
    return FALSE;
  }

  //
  // Check for broadcast, the caller has computed the packet's
  // cast type for this child's interface.
  //
  Info = IP4_GET_CLIP_INFO (Packet);

  if (IP4_IS_BROADCAST (Info->CastType)) {
    return Config->AcceptBroadcast;
  }

  //
  // If it is a multicast packet, check whether we are in the group.
  //
  if (Info->CastType == IP4_MULTICAST) {
    //
    // Receive the multicast if the instance wants to receive all packets.
    //
    if (!IpInstance->ConfigData.UseDefaultAddress && (IpInstance->Interface->Ip == 0)) {
      return TRUE;
    }

    for (Index = 0; Index < IpInstance->GroupCount; Index++) {
      if (IpInstance->Groups[Index] == HTONL (Head->Dst)) {
        break;
      }
    }

    return (BOOLEAN)(Index < IpInstance->GroupCount);
  }

  return TRUE;
}


/**
  Enqueue a shared copy of the packet to the IP4 child if the
  packet is acceptable to it. Here the data of the packet is
  shared, but the net buffer isn't.

  @param  IpInstance             The IP4 child to enqueue the packet to
  @param  Head                   The IP header of the received packet
  @param  Packet                 The data of the received packet

  @retval EFI_NOT_STARTED        The IP child hasn't been configured.
  @retval EFI_INVALID_PARAMETER  The child doesn't want to receive the packet
  @retval EFI_OUT_OF_RESOURCES   Failed to allocate some resource
  @retval EFI_SUCCESS            A shared copy the packet is enqueued to the child.

**/
EFI_STATUS
Ip4InstanceEnquePacket (
  IN IP4_PROTOCOL           *IpInstance,
  IN IP4_HEAD               *Head,
  IN NET_BUF                *Packet
  )
{
  IP4_CLIP_INFO             *Info;
  NET_BUF                   *Clone;

  //
  // Check whether the packet is acceptable to this instance.
  //
  if (IpInstance->State != IP4_STATE_CONFIGED) {
    return EFI_NOT_STARTED;
  }

  if (!Ip4InstanceFrameAcceptable (IpInstance, Head, Packet)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Enque a shared copy of the packet.
  //
  Clone = NetbufClone (Packet);

  if (Clone == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Set the receive time out for the assembled packet. If it expires,
  // packet will be removed from the queue.
  //
  Info        = IP4_GET_CLIP_INFO (Clone);
  Info->Life  = IP4_US_TO_SEC (IpInstance->ConfigData.ReceiveTimeout);

  InsertTailList (&IpInstance->Received, &Clone->List);
  return EFI_SUCCESS;
}


/**
  The signal handle of IP4's recycle event. It is called back
  when the upper layer release the packet.

  @param  Event                  The IP4's recycle event.
  @param  Context                The context of the handle, which is a
                                 IP4_RXDATA_WRAP

  @return None

**/
STATIC
VOID
EFIAPI
Ip4OnRecyclePacket (
  IN EFI_EVENT              Event,
  IN VOID                   *Context
  )
{
  IP4_RXDATA_WRAP           *Wrap;

  Wrap = (IP4_RXDATA_WRAP *) Context;

  EfiAcquireLockOrFail (&Wrap->IpInstance->RecycleLock);
  RemoveEntryList (&Wrap->Link);
  EfiReleaseLock (&Wrap->IpInstance->RecycleLock);

  ASSERT (!NET_BUF_SHARED (Wrap->Packet));
  NetbufFree (Wrap->Packet);

  gBS->CloseEvent (Wrap->RxData.RecycleSignal);
  gBS->FreePool (Wrap);
}


/**
  Wrap the received packet to a IP4_RXDATA_WRAP, which will be
  delivered to the upper layer. Each IP4 child that accepts the
  packet will get a not-shared copy of the packet which is wrapped
  in the IP4_RXDATA_WRAP. The IP4_RXDATA_WRAP->RxData is passed
  to the upper layer. Upper layer will signal the recycle event in
  it when it is done with the packet.

  @param  IpInstance             The IP4 child to receive the packet
  @param  Packet                 The packet to deliver up.

  @return NULL if failed to wrap the packet, otherwise the wrapper.

**/
IP4_RXDATA_WRAP *
Ip4WrapRxData (
  IN IP4_PROTOCOL           *IpInstance,
  IN NET_BUF                *Packet
  )
{
  IP4_RXDATA_WRAP           *Wrap;
  EFI_IP4_RECEIVE_DATA      *RxData;
  EFI_STATUS                Status;

  Wrap = AllocatePool (IP4_RXDATA_WRAP_SIZE (Packet->BlockOpNum));

  if (Wrap == NULL) {
    return NULL;
  }

  InitializeListHead (&Wrap->Link);

  Wrap->IpInstance  = IpInstance;
  Wrap->Packet      = Packet;
  RxData            = &Wrap->RxData;

  ZeroMem (&RxData->TimeStamp, sizeof (EFI_TIME));

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  Ip4OnRecyclePacket,
                  Wrap,
                  &RxData->RecycleSignal
                  );

  if (EFI_ERROR (Status)) {
    gBS->FreePool (Wrap);
    return NULL;
  }

  ASSERT (Packet->Ip != NULL);

  //
  // The application expects a network byte order header.
  //
  RxData->HeaderLength  = (Packet->Ip->HeadLen << 2);
  RxData->Header        = (EFI_IP4_HEADER *) Ip4NtohHead (Packet->Ip);

  RxData->OptionsLength = RxData->HeaderLength - IP4_MIN_HEADLEN;
  RxData->Options       = NULL;

  if (RxData->OptionsLength != 0) {
    RxData->Options = (VOID *) (RxData->Header + 1);
  }

  RxData->DataLength  = Packet->TotalSize;

  //
  // Build the fragment table to be delivered up.
  //
  RxData->FragmentCount = Packet->BlockOpNum;
  NetbufBuildExt (Packet, (NET_FRAGMENT *) RxData->FragmentTable, &RxData->FragmentCount);

  return Wrap;
}


/**
  Deliver the received packets to upper layer if there are both received
  requests and enqueued packets. If the enqueued packet is shared, it will
  duplicate it to a non-shared packet, release the shared packet, then
  deliver the non-shared packet up.

  @param  IpInstance             The IP child to deliver the packet up.

  @retval EFI_OUT_OF_RESOURCES   Failed to allocate resources to deliver the
                                 packets.
  @retval EFI_SUCCESS            All the enqueued packets that can be delivered
                                 are delivered up.

**/
EFI_STATUS
Ip4InstanceDeliverPacket (
  IN IP4_PROTOCOL           *IpInstance
  )
{
  EFI_IP4_COMPLETION_TOKEN  *Token;
  IP4_RXDATA_WRAP           *Wrap;
  NET_BUF                   *Packet;
  NET_BUF                   *Dup;
  UINT8                     *Head;

  //
  // Deliver a packet if there are both a packet and a receive token.
  //
  while (!IsListEmpty (&IpInstance->Received) &&
         !NetMapIsEmpty (&IpInstance->RxTokens)) {

    Packet = NET_LIST_HEAD (&IpInstance->Received, NET_BUF, List);

    if (!NET_BUF_SHARED (Packet)) {
      //
      // If this is the only instance that wants the packet, wrap it up.
      //
      Wrap = Ip4WrapRxData (IpInstance, Packet);

      if (Wrap == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }

      RemoveEntryList (&Packet->List);

    } else {
      //
      // Create a duplicated packet if this packet is shared
      //
      Dup = NetbufDuplicate (Packet, NULL, IP4_MAX_HEADLEN);

      if (Dup == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }

      //
      // Copy the IP head over. The packet to deliver up is
      // headless. Trim the head off after copy. The IP head
      // may be not continuous before the data.
      //
      Head    = NetbufAllocSpace (Dup, IP4_MAX_HEADLEN, NET_BUF_HEAD);
      Dup->Ip = (IP4_HEAD *) Head;

      CopyMem (Head, Packet->Ip, Packet->Ip->HeadLen << 2);
      NetbufTrim (Dup, IP4_MAX_HEADLEN, TRUE);

      Wrap = Ip4WrapRxData (IpInstance, Dup);

      if (Wrap == NULL) {
        NetbufFree (Dup);
        return EFI_OUT_OF_RESOURCES;
      }

      RemoveEntryList (&Packet->List);
      NetbufFree (Packet);

      Packet = Dup;
    }

    //
    // Insert it into the delivered packet, then get a user's
    // receive token, pass the wrapped packet up.
    //
    EfiAcquireLockOrFail (&IpInstance->RecycleLock);
    InsertHeadList (&IpInstance->Delivered, &Wrap->Link);
    EfiReleaseLock (&IpInstance->RecycleLock);

    Token                = NetMapRemoveHead (&IpInstance->RxTokens, NULL);
    Token->Status        = IP4_GET_CLIP_INFO (Packet)->Status;
    Token->Packet.RxData = &Wrap->RxData;

    gBS->SignalEvent (Token->Event);
  }

  return EFI_SUCCESS;
}


/**
  Enqueue a received packet to all the IP children that share
  the same interface.

  @param  IpSb                   The IP4 service instance that receive the packet
  @param  Head                   The header of the received packet
  @param  Packet                 The data of the received packet
  @param  IpIf                   The interface to enqueue the packet to

  @return The number of the IP4 children that accepts the packet

**/
INTN
Ip4InterfaceEnquePacket (
  IN IP4_SERVICE            *IpSb,
  IN IP4_HEAD               *Head,
  IN NET_BUF                *Packet,
  IN IP4_INTERFACE          *IpIf
  )
{
  IP4_PROTOCOL              *IpInstance;
  IP4_CLIP_INFO             *Info;
  LIST_ENTRY                *Entry;
  INTN                      Enqueued;
  INTN                      LocalType;
  INTN                      SavedType;

  //
  // First, check that the packet is acceptable to this interface
  // and find the local cast type for the interface. A packet sent
  // to say 192.168.1.1 should NOT be delliever to 10.0.0.1 unless
  // promiscuous receiving.
  //
  LocalType = 0;
  Info      = IP4_GET_CLIP_INFO (Packet);

  if ((Info->CastType == IP4_MULTICAST) || (Info->CastType == IP4_LOCAL_BROADCAST)) {
    //
    // If the CastType is multicast, don't need to filter against
    // the group address here, Ip4InstanceFrameAcceptable will do
    // that later.
    //
    LocalType = Info->CastType;

  } else {
    //
    // Check the destination againist local IP. If the station
    // address is 0.0.0.0, it means receiving all the IP destined
    // to local non-zero IP. Otherwise, it is necessary to compare
    // the destination to the interface's IP address.
    //
    if (IpIf->Ip == IP4_ALLZERO_ADDRESS) {
      LocalType = IP4_LOCAL_HOST;

    } else {
      LocalType = Ip4GetNetCast (Head->Dst, IpIf);

      if ((LocalType == 0) && IpIf->PromiscRecv) {
        LocalType = IP4_PROMISCUOUS;
      }
    }
  }

  if (LocalType == 0) {
    return 0;
  }

  //
  // Iterate through the ip instances on the interface, enqueue
  // the packet if filter passed. Save the original cast type,
  // and pass the local cast type to the IP children on the
  // interface. The global cast type will be restored later.
  //
  SavedType       = Info->CastType;
  Info->CastType  = LocalType;

  Enqueued        = 0;

  NET_LIST_FOR_EACH (Entry, &IpIf->IpInstances) {
    IpInstance = NET_LIST_USER_STRUCT (Entry, IP4_PROTOCOL, AddrLink);
    NET_CHECK_SIGNATURE (IpInstance, IP4_PROTOCOL_SIGNATURE);

    if (Ip4InstanceEnquePacket (IpInstance, Head, Packet) == EFI_SUCCESS) {
      Enqueued++;
    }
  }

  Info->CastType = SavedType;
  return Enqueued;
}


/**
  Deliver the packet for each IP4 child on the interface.

  @param  IpSb                   The IP4 service instance that received the packet
  @param  IpIf                   The IP4 interface to deliver the packet.

  @retval EFI_SUCCESS            It always returns EFI_SUCCESS now

**/
EFI_STATUS
Ip4InterfaceDeliverPacket (
  IN IP4_SERVICE            *IpSb,
  IN IP4_INTERFACE          *IpIf
  )
{
  IP4_PROTOCOL              *Ip4Instance;
  LIST_ENTRY                *Entry;

  NET_LIST_FOR_EACH (Entry, &IpIf->IpInstances) {
    Ip4Instance = NET_LIST_USER_STRUCT (Entry, IP4_PROTOCOL, AddrLink);
    Ip4InstanceDeliverPacket (Ip4Instance);
  }

  return EFI_SUCCESS;
}


/**
  Demultiple the packet. the packet delivery is processed in two
  passes. The first pass will enque a shared copy of the packet
  to each IP4 child that accepts the packet. The second pass will
  deliver a non-shared copy of the packet to each IP4 child that
  has pending receive requests. Data is copied if more than one
  child wants to consume the packet bacause each IP child need
  its own copy of the packet to make changes.

  @param  IpSb                   The IP4 service instance that received the packet
  @param  Head                   The header of the received packet
  @param  Packet                 The data of the received packet

  @retval EFI_NOT_FOUND          No IP child accepts the packet
  @retval EFI_SUCCESS            The packet is enqueued or delivered to some IP
                                 children.

**/
EFI_STATUS
Ip4Demultiplex (
  IN IP4_SERVICE            *IpSb,
  IN IP4_HEAD               *Head,
  IN NET_BUF                *Packet
  )
{
  LIST_ENTRY                *Entry;
  IP4_INTERFACE             *IpIf;
  INTN                      Enqueued;

  //
  // Two pass delivery: first, enque a shared copy of the packet
  // to each instance that accept the packet.
  //
  Enqueued = 0;

  NET_LIST_FOR_EACH (Entry, &IpSb->Interfaces) {
    IpIf = NET_LIST_USER_STRUCT (Entry, IP4_INTERFACE, Link);

    if (IpIf->Configured) {
      Enqueued += Ip4InterfaceEnquePacket (IpSb, Head, Packet, IpIf);
    }
  }

  //
  // Second: deliver a duplicate of the packet to each instance.
  // Release the local reference first, so that the last instance
  // getting the packet will not copy the data.
  //
  NetbufFree (Packet);

  if (Enqueued == 0) {
    return EFI_NOT_FOUND;
  }

  NET_LIST_FOR_EACH (Entry, &IpSb->Interfaces) {
    IpIf = NET_LIST_USER_STRUCT (Entry, IP4_INTERFACE, Link);

    if (IpIf->Configured) {
      Ip4InterfaceDeliverPacket (IpSb, IpIf);
    }
  }

  return EFI_SUCCESS;
}


/**
  Timeout the fragment and enqueued packets.

  @param  IpSb                   The IP4 service instance to timeout

  @return None

**/
VOID
Ip4PacketTimerTicking (
  IN IP4_SERVICE            *IpSb
  )
{
  LIST_ENTRY                *InstanceEntry;
  LIST_ENTRY                *Entry;
  LIST_ENTRY                *Next;
  IP4_PROTOCOL              *IpInstance;
  IP4_ASSEMBLE_ENTRY        *Assemble;
  NET_BUF                   *Packet;
  IP4_CLIP_INFO             *Info;
  UINT32                    Index;

  //
  // First, time out the fragments. The packet's life is counting down
  // once the first-arrived fragment was received.
  //
  for (Index = 0; Index < IP4_ASSEMLE_HASH_SIZE; Index++) {
    NET_LIST_FOR_EACH_SAFE (Entry, Next, &IpSb->Assemble.Bucket[Index]) {
      Assemble = NET_LIST_USER_STRUCT (Entry, IP4_ASSEMBLE_ENTRY, Link);

      if ((Assemble->Life > 0) && (--Assemble->Life == 0)) {
        RemoveEntryList (Entry);
        Ip4FreeAssembleEntry (Assemble);
      }
    }
  }

  NET_LIST_FOR_EACH (InstanceEntry, &IpSb->Children) {
    IpInstance = NET_LIST_USER_STRUCT (InstanceEntry, IP4_PROTOCOL, Link);

    //
    // Second, time out the assembled packets enqueued on each IP child.
    //
    NET_LIST_FOR_EACH_SAFE (Entry, Next, &IpInstance->Received) {
      Packet = NET_LIST_USER_STRUCT (Entry, NET_BUF, List);
      Info   = IP4_GET_CLIP_INFO (Packet);

      if ((Info->Life > 0) && (--Info->Life == 0)) {
        RemoveEntryList (Entry);
        NetbufFree (Packet);
      }
    }

    //
    // Third: time out the transmitted packets.
    //
    NetMapIterate (&IpInstance->TxTokens, Ip4SentPacketTicking, NULL);
  }
}
