/* test_headers.c - every public module header must compile together, in any
 * order, under C89 (-pedantic) and C23. Nothing is executed. */
#include "lt.h"
#include "lt_codes.h"
#include "plat/plat.h"
#include "gui/gui.h"
#include "ini/expr.h"
#include "ini/ini.h"
#include "xml/xml.h"
#include "json/json.h"
#include "tableops/tableops.h"
#include "proto/proto.h"
#include "tune/tune.h"
#include "datalog/datalog.h"
#include "autotune/autotune.h"
#include "basemap/basemap.h"
#include "sim/sim.h"
#include "dash/dash.h"
#include "project/project.h"
#include "lt_test.h"

LT_TEST(headers_compile)
{
    /* a few sizeof/constant touches so the includes are not optimised away
     * by the preprocessor-only view of the world */
    LT_CHECK(sizeof(lt_ini) > 0);
    LT_CHECK(sizeof(lt_proto_io) > 0);
    LT_CHECK(sizeof(lt_tune) > 0);
    LT_CHECK(LT_BASEMAP_N == 16);
    LT_CHECK(LT_CALIB_ADC_COUNTS == 1024);
}

int main(void)
{
    LT_RUN(headers_compile);
    LT_TEST_MAIN_END
}
