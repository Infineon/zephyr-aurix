#include <zephyr/init.h>
#include <zephyr/devicetree.h>

#include "soc_prot.h"
#include "soc_tagid.h"

#include "IfxCpu_reg.h"

int z_soc_tc4x_apu_setup()
{
#if DT_HAS_COMPAT_STATUS_OKAY(snps_dwc_ether_xgmac)
	aurix_apu_enable_write_select(&CPU0_PROTSPRSE, &CPU0_ACCENSPRCFG_WRA, 0,
				      AURIX_TAGID_GETH_C0);
	aurix_apu_enable_write_select(&CPU0_PROTSPRSE, &CPU0_ACCENSPRCFG_WRA, 0,
				      AURIX_TAGID_GETH_C1);
#endif

	return 0;
}
SYS_INIT(z_soc_tc4x_apu_setup, POST_KERNEL, 10);
