import struct

def parse_xm(filename):
    with open(filename, "rb") as f:
        data = f.read()

    offset = 60
    header_size = struct.unpack("<I", data[offset:offset+4])[0]
    pot_length = struct.unpack("<H", data[offset+4:offset+6])[0]
    num_channels = struct.unpack("<H", data[offset+8:offset+10])[0]
    num_patterns = struct.unpack("<H", data[offset+10:offset+12])[0]
    num_instruments = struct.unpack("<H", data[offset+12:offset+14])[0]
    
    offset += header_size
    
    total_rows = 0
    for i in range(num_patterns):
        header_len = struct.unpack("<I", data[offset:offset+4])[0]
        pack_type = data[offset+4]
        num_rows = struct.unpack("<H", data[offset+5:offset+7])[0]
        packed_size = struct.unpack("<H", data[offset+7:offset+9])[0]
        
        if packed_size == 0 and num_rows != 64:
            num_rows = 64
            
        total_rows += num_rows
        offset += header_len + packed_size
        
    print(f"Total rows: {total_rows}")
    
    total_sample_data_length = 0
    num_samples = 0
    
    for i in range(num_instruments):
        inst_size = struct.unpack("<I", data[offset:offset+4])[0]
        num_samples_inst = struct.unpack("<H", data[offset+27:offset+29])[0]
        num_samples += num_samples_inst
        
        inst_offset = offset + inst_size
        
        inst_sample_bytes = 0
        
        if num_samples_inst > 0:
            sample_header_size = struct.unpack("<I", data[offset+29:offset+33])[0]
            
            for j in range(num_samples_inst):
                sample_length = struct.unpack("<I", data[inst_offset:inst_offset+4])[0]
                flags = data[inst_offset+14]
                
                inst_sample_bytes += sample_length
                
                if flags & 0x10:
                    sample_length //= 2
                    
                total_sample_data_length += sample_length
                inst_offset += sample_header_size
                
        offset = inst_offset + inst_sample_bytes
        
    print(f"Num samples: {num_samples}")
    print(f"Total sample data length: {total_sample_data_length}")

if __name__ == "__main__":
    parse_xm("main/roadblast.xm")
