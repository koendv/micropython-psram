#include "py/obj.h"
#include "py/objarray.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/mpconfig.h"
#include "psram.h"

#if defined(MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2)

// -----------------------------------------------------------------------------
// module definitions

#define mp_raise_RuntimeError(msg) (mp_raise_msg(&mp_type_RuntimeError, (msg)))

STATIC mp_obj_t mp_psram_init();
STATIC mp_obj_t mp_psram_read(mp_obj_t addr_obj, mp_obj_t data_obj);
STATIC mp_obj_t mp_psram_write(mp_obj_t addr_obj, mp_obj_t data_obj);

STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_psram_init_obj, mp_psram_init);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_psram_read_obj, mp_psram_read);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mp_psram_write_obj, mp_psram_write);

STATIC const mp_rom_map_elem_t psram_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_psram)},
    {MP_ROM_QSTR(MP_QSTR___init__), MP_ROM_PTR(&mp_psram_init_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_psram_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_psram_write_obj)},
};

MP_DEFINE_CONST_DICT(psram_globals_dict, psram_module_globals_table);

typedef struct _mp_obj_psram_t {
    mp_obj_base_t base;
} mp_obj_psram_t;

const mp_obj_module_t psram_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&psram_globals_dict,
};

// Register the module to make it available in Python
MP_REGISTER_MODULE(MP_QSTR_psram, psram_module, (1));

// -----------------------------------------------------------------------------
// code

/* access to bytes/bytearray contents */

/*
   o: micropython string, bytes or bytearray (input)
   len: length of string, bytes or bytearray (output)
   items: output, string, bytes or bytearray data (output)
 */

static void mp_obj_get_data(mp_obj_t o, size_t *len, mp_obj_t **items) {
    if (mp_obj_is_type(MP_OBJ_FROM_PTR(o), &mp_type_bytearray)) {
        mp_obj_array_t *barray = MP_OBJ_FROM_PTR(o);
        *len = barray->len;
        *items = barray->items;
        return;
    }
    if (mp_obj_is_type(MP_OBJ_FROM_PTR(o), &mp_type_bytes) ||
        mp_obj_is_type(MP_OBJ_FROM_PTR(o), &mp_type_str)) {
        mp_obj_str_t *str = MP_OBJ_FROM_PTR(o);
        *len = str->len;
        *items = (void *)str->data;
        return;
    }
    mp_raise_TypeError(MP_ERROR_TEXT("object not string, bytes nor bytearray"));
}

STATIC mp_obj_t mp_psram_init() {
    psram_init();
    return mp_const_none;
}

STATIC mp_obj_t mp_psram_read(mp_obj_t addr_obj, mp_obj_t dest_obj) {
    uint32_t addr = mp_obj_get_int(addr_obj);
    size_t len;
    mp_obj_t *dest;
    mp_obj_get_data(dest_obj, &len, &dest);
    if (len != 0) {
        psram_read(addr, len, (uint8_t *)dest);
    }
    return mp_const_none;
};

STATIC mp_obj_t mp_psram_write(mp_obj_t addr_obj, mp_obj_t src_obj) {
    uint32_t addr = mp_obj_get_int(addr_obj);
    size_t len;
    mp_obj_t *src;
    mp_obj_get_data(src_obj, &len, &src);
    if (len != 0) {
        psram_write(addr, len, (uint8_t *)src);
    }
    return mp_const_none;
};

#endif // defined(MICROPY_HW_QSPIRAM_SIZE_BITS_LOG2)
// not truncated
