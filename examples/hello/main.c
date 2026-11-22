#include "wada_boot.h"
#include "wada_store_sqlite.h"

extern const wada_product hello_product;

/* An example app is a product table plus one boot call. */
int main(int argc, char **argv) {
    const wada_product *products[] = { &hello_product };
    wada_boot_desc desc = {
        "Wada — Hello",
        900,
        620,
        false,
        wada_store_sqlite(),
        products,
        1,
        "hello",
        "Wada Hello",
    };
    return wada_boot_run(&desc, argc, argv);
}
