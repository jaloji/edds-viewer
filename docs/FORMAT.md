# EDDS format notes

What the reader in `src/core/edds.cpp` implements. All values are little-endian.

Reference: [WoozyMasta/edds](https://github.com/WoozyMasta/edds) (MIT), whose Workbench-made sample files were used for testing.

## Layout

```
char[4]       "DDS "
DDS_HEADER    124 bytes, standard; dwMipMapCount gives the number of blocks
DDS_HEADER_DXT10   20 bytes, only when the pixel format FourCC is "DX10"
BLOCK_ENTRY[mipMapCount]
u8[]          block bodies, in the same order as the entries
```

The blocks go from the **smallest** mipmap to the **largest**, the reverse of a DDS file.

### Block entry

```
char[4]  magic   "COPY" (raw) or "LZ4 " (compressed)
i32      size    size of the block body in bytes
```

### COPY body

The raw mipmap data, exactly as in a DDS file.

### LZ4 body

```
u32      uncompressed size of the mipmap
CHUNK[]  until a chunk with the "last" flag
```

Each chunk:

```
u24      compressed size
u8       flags: 0x80 = last chunk, other bits 0
u8[]     standard LZ4 block
```

Every chunk decodes to 64 KiB, except the last one. A chunk may copy from the previous 64 KiB of output, including data from earlier chunks, so the whole mipmap is decoded into one contiguous buffer.

## Rebuilding a DDS

Keep the DDS header (and DX10 header), then append the unpacked mipmaps from largest to smallest. The result is a standard DDS file.

Files without a block table after the header are read as plain DDS.

## Channel swizzles

Workbench can store channels shuffled (normal maps, SMDI, terrain textures). Unlike PAA, the file does not say which profile was used, so viewers show the stored channels.
