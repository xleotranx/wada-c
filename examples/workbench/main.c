#include "wb_store.h"
#include "wada_boot.h"
#include "wada_store_sqlite.h"

extern const wada_product wb_issues_product;
extern const wada_product wb_activity_product;

int main(int argc, char **argv) {
    const wada_product *products[] = { &wb_issues_product, &wb_activity_product };
    wada_boot_desc desc = {
        "Wada — Workbench",
        1200,
        780,
        false,
        wada_store_sqlite(),
        products,
        2,
        "wb.issues",
        "Wada Workbench",
        wb_setup,
    };
    return wada_boot_run(&desc, argc, argv);
}
