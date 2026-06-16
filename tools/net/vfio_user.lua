-- VFIO-user Protocol Dissector for Wireshark
-- This Lua script implements a dissector for the VFIO-user protocol over Unix Domain Sockets.
--
-- The dissector parses the VFIO-user message structure, which consists of a 16-byte header followed by an optional payload.
-- It handless message fragmentation by buffering incomplete messages and reassembling them when the remaining fragments arrive.
--
-- High Level Arch Diagram of the setup using this dissector:
-- (QEMU vfio-user-pci) + Linux OS
--  |
--  +- vfio-user UDS proto -> (vfio-user Server) <= + PCIe Bridge + => (UDS/TCP Client) +
--              ^                                   |   Cosim     |                     |
--              |                                            soft-TLP proto             + <= UDS/TCP => verilated PCIe Sim (UDS/TCP Server) Endpoint
--   sockdump.py sniffer                      libvfio-user          backend
--   captures vfio-user proto                 frontend
--   msgs into pcap fmt
--
-- The dissector is registered for the DLT_USER0 encapsulation type,
-- which is used by the 'sockdump.py' tool when capturing VFIO-user messages.
--
-- Copyright (c) 2026, Purple
-- This file is licensed under the MIT License.
--
local vfio_user_proto = Proto("vfio_user", "VFIO-user Protocol")

local errno_str = {
    [0]  = "SUCCESS",
    [1]  = "EPERM (Operation not permitted)",
    [2]  = "ENOENT (Invalid Address Range/No such region)",
    [4]  = "EINTR (Interrupted system call)",
    [5]  = "EIO (Input/Output error)",
    [11] = "EAGAIN (Resource temporarily unavailable)",
    [12] = "ENOMEM (Out of memory)",
    [13] = "EACCES (Permission denied)",
    [14] = "EFAULT (Bad address block)",
    [16] = "EBUSY (Device or resource busy)",
    [22] = "EINVAL (Invalid argument passed)"
}

local cmd_names = {
    [1]  = "VERSION",
    [2]  = "DMA_MAP",
    [3]  = "DMA_UNMAP",
    [4]  = "DEVICE_GET_INFO",
    [5]  = "DEVICE_GET_REGION_INFO",
    [6]  = "DEVICE_GET_REGION_IO_FDS",
    [7]  = "DEVICE_GET_IRQ_INFO",
    [8]  = "DEVICE_SET_IRQS",
    [9]  = "REGION_READ",
    [10] = "REGION_WRITE",
    [11] = "DMA_READ",
    [12] = "DMA_WRITE",
    [13] = "DEVICE_RESET",
    [15] = "REGION_WRITE_MULTI",
    [16] = "DEVICE_FEATURE",
    [17] = "MIG_DATA_READ",
    [18] = "MIG_DATA_WRITE"
}

local f_id    = ProtoField.uint16("vfio.id", "Vfio Id", base.HEX)
local f_cmd   = ProtoField.uint16("vfio.cmd", "Cmd", base.DEC, cmd_names)
local f_size  = ProtoField.uint32("vfio.size", "Size", base.DEC)
local f_flags = ProtoField.uint32("vfio.flags", "Flags", base.HEX)
local f_error = ProtoField.uint32("vfio.error", "Error No", base.DEC)

local f_reg_off  = ProtoField.uint64("vfio.reg.offset", "Offset", base.HEX)
local f_reg_idx  = ProtoField.uint32("vfio.reg.index", "Region Index", base.DEC)
local f_reg_cnt  = ProtoField.uint32("vfio.reg.count", "Byte Count", base.DEC)
local f_payload  = ProtoField.bytes("vfio.payload", "Data Payload")
local f_dma_gpa  = ProtoField.uint64("vfio.dma.gpa", "Guest Physical Address", base.HEX)
local f_dma_size = ProtoField.uint64("vfio.dma.size", "Region Length", base.DEC)
local f_dma_off  = ProtoField.uint64("vfio.dma.offset", "File Offset", base.HEX)
local f_irq_index = ProtoField.uint32("vfio.irq.index", "IRQ Index", base.DEC)
local f_irq_count = ProtoField.uint32("vfio.irq.count", "IRQ Count", base.DEC)
local f_irq_start = ProtoField.uint32("vfio.irq.start", "IRQ Start Sub-index", base.DEC)
local f_irq_flags = ProtoField.uint32("vfio.irq.flags", "IRQ Flags", base.HEX)
local f_dev_num_regions = ProtoField.uint32("vfio.dev.num_regions", "Max Regions", base.DEC)
local f_dev_num_irqs    = ProtoField.uint32("vfio.dev.num_irqs", "Max IRQ Types", base.DEC)
local f_dev_flags       = ProtoField.uint32("vfio.dev.flags", "Device Flags", base.HEX)
local f_feat_flags = ProtoField.uint32("vfio.feat.flags", "Feature Flags", base.HEX)
local f_feat_idx   = ProtoField.uint32("vfio.feat.index", "Feature Index", base.DEC)

vfio_user_proto.fields = {
    f_id, f_cmd, f_size, f_flags, f_error, f_reg_off, f_reg_idx, f_reg_cnt, f_payload,
    f_dma_gpa, f_dma_size, f_dma_off,
    f_irq_index, f_irq_count, f_irq_start, f_irq_flags,
    f_dev_num_regions, f_dev_num_irqs, f_dev_flags,
    f_feat_flags, f_feat_idx
}

local packet_states = {}
local frag_hdr = nil

local function parse_proto_fields(tvbuf, pktinfo, root, offset, length)
    local tree = root:add(vfio_user_proto, tvbuf(offset, length))

    local msg_id   = tvbuf(offset + 0, 2):le_uint()
    local cmd      = tvbuf(offset + 2, 2):le_uint()
    local msg_size = tvbuf(offset + 4, 4):le_uint()
    local flags    = tvbuf(offset + 8, 1):uint()
    local err_no   = tvbuf(offset + 12, 4):le_uint()
    local cmd_name = cmd_names[cmd] or string.format("UNKNOWN(0x%X)", cmd)

    local is_reply = force_reply_state
    if is_reply == nil then
        is_reply = ((flags & 1) == 1) or (flags % 2 == 1)
    end

    if is_reply then
        pktinfo.cols.src:set("Server")
        pktinfo.cols.dst:set("Client")
    else
        pktinfo.cols.src:set("Client")
        pktinfo.cols.dst:set("Server")
    end
    pktinfo.cols.src:fence()
    pktinfo.cols.dst:fence()

    pktinfo.cols.protocol:set("VFIO-USER")
    pktinfo.cols.protocol:fence()

    tree:add_le(f_id,   tvbuf(offset + 0, 2))
    tree:add_le(f_cmd,  tvbuf(offset + 2, 2))
    tree:add_le(f_size, tvbuf(offset + 4, 4))
    local f_flags_tree = tree:add_le(f_flags, tvbuf(offset + 8, 4))
    f_flags_tree:add(tvbuf(offset + 8, 4), "Type: " .. (is_reply and "Reply" or "Command"))
    tree:add_le(f_error, tvbuf(offset + 12, 4))

    local available_payload = length - 16 -- VFIO-User Protocol Header is 16 bytes
    if available_payload > 0 then
        if cmd == 2 or cmd == 3 then -- DMA_MAP (2) or DMA_UNMAP (3)
            if available_payload >= 24 then
                local dma_tree = tree:add(tvbuf(offset + 16, 24), "DMA Memory Map Data")
                dma_tree:add_le(f_dma_gpa,  tvbuf(offset + 16, 8)) -- Guest Physical Address Address range
                dma_tree:add_le(f_dma_size, tvbuf(offset + 24, 8)) -- Total Region Length Block size
                dma_tree:add_le(f_dma_off,  tvbuf(offset + 32, 8)) -- Shared Memory Map Offset point
                if available_payload > 24 then
                    tree:add(f_payload, tvbuf(offset + 40, available_payload - 24))
                end
            else
                tree:add(f_payload, tvbuf(offset + 16, available_payload))
            end
        elseif cmd == 4 then -- DEVICE_GET_INFO (4)
            if available_payload >= 16 then
                local dev_tree = tree:add(tvbuf(offset + 16, 16), "Device Information Payload")
                dev_tree:add_le(f_size, tvbuf(offset + 16, 4))
                local dev_flags = tvbuf(offset + 20, 4):le_uint()
                local dev_desc = {}
                if (dev_flags & 0x01) ~= 0 then table.insert(dev_desc, "RESET") end
                if (dev_flags & 0x02) ~= 0 then table.insert(dev_desc, "PCI_CLS") end
                if (dev_flags & 0x04) ~= 0 then table.insert(dev_desc, "PLATFORM") end
                if (dev_flags & 0x08) ~= 0 then table.insert(dev_desc, "AMBA") end
                if (dev_flags & 0x10) ~= 0 then table.insert(dev_desc, "CCW") end
                if (dev_flags & 0x20) ~= 0 then table.insert(dev_desc, "AP") end
                if (dev_flags & 0x40) ~= 0 then table.insert(dev_desc, "FSL_MC") end
                if (dev_flags & 0x80) ~= 0 then table.insert(dev_desc, "CAPS") end
                local dev_str = (#dev_desc > 0) and table.concat(dev_desc, "|") or "NONE"
                local dev_flags_tree = dev_tree:add_le(f_dev_flags, tvbuf(offset + 20, 4))
                dev_flags_tree:add(tvbuf(offset + 20, 4), "Decoded Caps: " .. dev_str)
                dev_tree:add_le(f_dev_num_regions, tvbuf(offset + 24, 4))
                dev_tree:add_le(f_dev_num_irqs,    tvbuf(offset + 28, 4))
                if available_payload > 16 then
                    tree:add(f_payload, tvbuf(offset + 32, available_payload - 16))
                end
            else
                tree:add(f_payload, tvbuf(offset + 16, available_payload))
            end
        elseif cmd == 7 then -- DEVICE_GET_IRQ_INFO (7)
            if available_payload >= 16 then
                local irq_tree = tree:add(tvbuf(offset + 16, 16), "IRQ Information Data")
                irq_tree:add_le(f_size, tvbuf(offset + 16, 4))

                local irq_flags = tvbuf(offset + 20, 4):le_uint()
                local flag_desc = {}
                if (irq_flags & 0x01) ~= 0 then table.insert(flag_desc, "EVENTFD") end
                if (irq_flags & 0x02) ~= 0 then table.insert(flag_desc, "MASKABLE") end
                if (irq_flags & 0x04) ~= 0 then table.insert(flag_desc, "AUTOMASKED") end
                if (irq_flags & 0x08) ~= 0 then table.insert(flag_desc, "NORESIZE") end
                local flag_str = (#flag_desc > 0) and table.concat(flag_desc, "|") or "NONE"
                local flags_sub_tree = irq_tree:add_le(f_irq_flags, tvbuf(offset + 20, 4))
                flags_sub_tree:add(tvbuf(offset + 20, 4), "Decoded: " .. flag_str)
                irq_tree:add_le(f_irq_index, tvbuf(offset + 24, 4)) -- IRQ type index
                irq_tree:add_le(f_irq_count, tvbuf(offset + 28, 4)) -- Number of interrupts supported
                if available_payload > 16 then
                    tree:add(f_payload, tvbuf(offset + 32, available_payload - 16))
                end
            else
                tree:add(f_payload, tvbuf(offset + 16, available_payload))
            end
        elseif cmd == 8 then -- DEVICE_SET_IRQS (8)
            if available_payload >= 20 then
                local irq_set_tree = tree:add(tvbuf(offset + 16, 20), "IRQ Action Configuration Data")
                irq_set_tree:add_le(f_size, tvbuf(offset + 16, 4))
                local set_flags = tvbuf(offset + 20, 4):le_uint()
                local set_desc = {}
                if (set_flags & 0x01) ~= 0 then table.insert(set_desc, "DATA_NONE") end
                if (set_flags & 0x02) ~= 0 then table.insert(set_desc, "DATA_BOOL") end
                if (set_flags & 0x04) ~= 0 then table.insert(set_desc, "DATA_EVENTFD") end
                if (set_flags & 0x08) ~= 0 then table.insert(set_desc, "ACTION_MASK") end
                if (set_flags & 0x10) ~= 0 then table.insert(set_desc, "ACTION_UNMASK") end
                if (set_flags & 0x20) ~= 0 then table.insert(set_desc, "ACTION_TRIGGER") end
                local set_str = (#set_desc > 0) and table.concat(set_desc, "|") or "NONE"
                local set_flags_tree = irq_set_tree:add_le(f_irq_flags, tvbuf(offset + 20, 4))
                set_flags_tree:add(tvbuf(offset + 20, 4), "Decoded: " .. set_str)
                irq_set_tree:add_le(f_irq_index, tvbuf(offset + 24, 4)) -- Type configuration index
                irq_set_tree:add_le(f_irq_start, tvbuf(offset + 28, 4)) -- Sub-index start target index
                irq_set_tree:add_le(f_irq_count, tvbuf(offset + 32, 4)) -- Count array scale

                if available_payload > 20 then
                    tree:add(f_payload, tvbuf(offset + 36, available_payload - 20))
                end
            else
                tree:add(f_payload, tvbuf(offset + 16, available_payload))
            end
        elseif cmd == 9 or cmd == 10 then -- REGION_READ (9) or REGION_WRITE (10)
            if available_payload >= 16 then
                local payload_tree = tree:add(tvbuf(offset + 16, 16), "Region Access Info")
                payload_tree:add_le(f_reg_off, tvbuf(offset + 16, 8))
                payload_tree:add_le(f_reg_idx, tvbuf(offset + 24, 4))
                payload_tree:add_le(f_reg_cnt, tvbuf(offset + 28, 4))
                if available_payload > 16 then
                    tree:add(f_payload, tvbuf(offset + 32, available_payload - 16))
                end
            else
                tree:add(f_payload, tvbuf(offset + 16, available_payload))
            end
        elseif cmd == 16 then -- DEVICE_FEATURE (16)
            if available_payload >= 12 then
                local feat_tree = tree:add(tvbuf(offset + 16, 12), "Device Feature Payload")
                feat_tree:add_le(f_size, tvbuf(offset + 16, 4))
                feat_tree:add_le(f_feat_idx, tvbuf(offset + 20, 4))
                local feat_flags = tvbuf(offset + 24, 4):le_uint()
                local feat_desc = {}
                local action_type = feat_flags & 0x07
                if action_type == 0 then table.insert(feat_desc, "GET")
                elseif action_type == 1 then table.insert(feat_desc, "SET")
                elseif action_type == 2 then table.insert(feat_desc, "PROBE")
                end
                local feat_str = (#feat_desc > 0) and table.concat(feat_desc, "|") or "RESERVED"
                local feat_flags_tree = feat_tree:add_le(f_feat_flags, tvbuf(offset + 24, 4))
                feat_flags_tree:add(tvbuf(offset + 24, 4), "Action Type: " .. feat_str)
                if available_payload > 12 then
                    tree:add(f_payload, tvbuf(offset + 28, available_payload - 12))
                end
            end
        else
            tree:add(f_payload, tvbuf(offset + 16, available_payload))
        end
    end

    local mode = is_reply and "RES" or "REQ"
    local status = ""
    if is_reply and err_no ~= 0 then
        local err_name = errno_str[err_no] or string.format("UNKNOWN(%d)", err_no)
        status = string.format(" ERR=%s", err_name)
    end
    pktinfo.cols.info:set(string.format("[%s] Id=0x%04X %s Size=%d%s",
                          mode, msg_id, cmd_name, msg_size, status))
    pktinfo.cols.info:fence()
end

local function process_msg_frags(tvbuf, pktinfo, pktlen, f_num)
    if pktinfo.visited then return end

    if frag_hdr and frag_hdr.expected > frag_hdr.buffer:len() then
        frag_hdr.buffer:append(tvbuf():bytes())

        if frag_hdr.buffer:len() >= frag_hdr.expected then
            packet_states[f_num] = {
                type = "REASSEMBLED",
                buffer = frag_hdr.buffer,
                length = frag_hdr.expected,
                is_reply = frag_hdr.is_reply
            }
            frag_hdr = nil
        else
            packet_states[f_num] = {
                type = "FRAG_MSG",
                buffer = frag_hdr.buffer,
                expected = frag_hdr.expected,
                is_reply = frag_hdr.is_reply
            }
        end
        return
    end

    if pktlen == 0 then
        packet_states[f_num] = { type = "ZERO_LEN" }
    elseif pktlen < 16 then
        packet_states[f_num] = { type = "SUB_LEN" }
    else
        local msg_size = tvbuf(4, 4):le_uint()
        local flags    = tvbuf(8, 4):le_uint()
        local reply_state = (flags & 1) == 1

        if pktlen < msg_size and msg_size <= 1048576 then -- 1MB max message size
            local new_buf = ByteArray.new()
            new_buf:append(tvbuf():bytes())
            packet_states[f_num] = {
                type = "FRAG_HDR",
                buffer = new_buf,
                expected = msg_size,
                is_reply = reply_state
            }
            frag_hdr = packet_states[f_num]
        else
            packet_states[f_num] = { type = "FULL_MSG", is_reply = reply_state }
        end
    end
end

function vfio_user_proto.dissector(tvbuf, pktinfo, root)
    local pktlen = tvbuf:len()
    local f_num = pktinfo.number

    process_msg_frags(tvbuf, pktinfo, pktlen, f_num)

    local state = packet_states[f_num]
    if not state or state.type == "ZERO_LEN" then
        pktinfo.cols.src:set("")
        pktinfo.cols.dst:set("")
        pktinfo.cols.protocol:set("VFIO-GAP")
        pktinfo.cols.info:set("[NONE]")
        pktinfo.cols.src:fence()
        pktinfo.cols.dst:fence()
        pktinfo.cols.protocol:fence()
        pktinfo.cols.info:fence()
        return 0
    end

    if state.type == "REASSEMBLED" then
        local msg_tvb = ByteArray.tvb(state.buffer, "Reassembled VFIO-User Message")
        force_reply_state = state.is_reply
        parse_proto_fields(msg_tvb, pktinfo, root, 0, state.length)
        force_reply_state = nil
        return pktlen
    end

    if state.type == "FRAG_MSG" then
        pktinfo.cols.protocol:set("VFIO-FRAG")
        pktinfo.cols.info:set(string.format("[Fragment] Received: %d/%d bytes",
                                            pktlen, state.expected))
        pktinfo.cols.protocol:fence()
        pktinfo.cols.info:fence()
        local tree = root:add(vfio_user_proto, tvbuf(0, pktlen))
        tree:add(f_payload, tvbuf(0, pktlen))
        return pktlen
    end

    local offset = 0
    local hdr_locked = false -- is header aligned and marked for parsing

    while offset < pktlen do
        local received = pktlen - offset
        if received < 16 then break end

        local msg_id   = tvbuf(offset + 0, 2):le_uint()
        local cmd      = tvbuf(offset + 2, 2):le_uint()
        local msg_size = tvbuf(offset + 4, 4):le_uint()
        local flags    = tvbuf(offset + 8, 4):le_uint()

        local is_valid_hdr = (cmd >= 1 and cmd <= 18 and cmd ~= 14) and
                             (msg_size >= 16 and msg_size <= 1048576) -- 1MB max message size

        if not is_valid_hdr then
            offset = offset + 1
        else
            hdr_locked = true
            if received < msg_size then
                if not pktinfo.visited and state.type == "FULL_MSG" then
                    local new_buf = ByteArray.new()
                    new_buf:append(tvbuf(offset, received):bytes())
                    state.type = "FRAG_HDR"
                    state.buffer = new_buf
                    state.expected = msg_size
                    state.is_reply = (flags & 1) == 1
                    frag_hdr = state
                end

                pktinfo.cols.protocol:set("VFIO-FRAG")
                local cmd_name = cmd_names[cmd] or "UNKNOWN"
                pktinfo.cols.info:set(string.format("[Fragment] %s (%d) Received: %d/%d bytes",
                                      cmd_name, cmd, received, msg_size))
                pktinfo.cols.protocol:fence()
                pktinfo.cols.info:fence()
                local tree = root:add(vfio_user_proto, tvbuf(offset, received))
                tree:add(f_payload, tvbuf(offset, received))
                offset = offset + received
                break
            end

            force_reply_state = state.is_reply
            parse_proto_fields(tvbuf, pktinfo, root, offset, msg_size)
            force_reply_state = nil
            offset = offset + msg_size
        end
    end

    if not hdr_locked and offset > 0 then
        pktinfo.cols.protocol:set("VFIO-GAP")
        pktinfo.cols.info:set(string.format("[Unaligned Segment] Length: %d bytes", pktlen))
        pktinfo.cols.protocol:fence()
        pktinfo.cols.info:fence()
        local pad_tree = root:add(vfio_user_proto, tvbuf(0, pktlen))
        pad_tree:set_text("Alignment Bytes")
        return pktlen
    end

    return offset
end

-- Register the dissector for the DLT_USER0 encapsulation type used
-- by sockdump.py and for any generic user-defined encapsulation
local wtap_encap_table = DissectorTable.get("wtap_encap")
if wtap_encap_table then wtap_encap_table:add(wtap.USER0, vfio_user_proto) end
local user_dlt_table = DissectorTable.get("user_dlts")
if user_dlt_table then user_dlt_table:add(147, vfio_user_proto) end
