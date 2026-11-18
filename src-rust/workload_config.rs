pub const CONFIG_MAGIC: &[u8; 8] = b"SVMMWL1\0";
pub const CONFIG_HEADER_SIZE: usize = CONFIG_MAGIC.len() + 2 * size_of::<u32>();
pub const CONFIG_MAX: usize = 1024 * 1024;
pub const ITEMS_MAX: usize = 4096;
