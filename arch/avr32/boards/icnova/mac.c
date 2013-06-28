
#include <linux/init.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <asm/setup.h>
#include <mach/board.h>

extern struct tag *bootloader_tags __initdata;

struct eth_addr {
	u8 addr[6];
};

static struct eth_addr __initdata hw_addr[2];
struct macb_platform_data __initdata eth_data[] = {
	{
		.phy_mask	= ~(1U << 0),
		.phy_irq_pin = -1,
	},
#ifdef CONFIG_BOARD_ICNOVA_DEV_MACB
	{
		.phy_mask	= ~(1U << 0),
		.phy_irq_pin = -1,
	},
#endif
};

/*
 * The next two functions should go away as the boot loader is
 * supposed to kize the macb address registers with a valid
 * ethernet address. But we need to keep it around for a while until
 * we can be reasonably sure the boot loader does this.
 *
 * The phy_id is ignored as the driver will probe for it.
 */
static int __init parse_tag_ethernet(struct tag *tag)
{
	int i;

	i = tag->u.ethernet.mac_index;
	if (i < ARRAY_SIZE(hw_addr))
		memcpy(hw_addr[i].addr, tag->u.ethernet.hw_address,
		       sizeof(hw_addr[i].addr));

	return 0;
}
__tagtable(ATAG_ETHERNET, parse_tag_ethernet);

void __init set_hw_addr(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	const u8 *addr;
	void __iomem *regs;
	struct clk *pclk;

	if (!res)
		return;
	if (pdev->id >= ARRAY_SIZE(hw_addr))
		return;

	addr = hw_addr[pdev->id].addr;
	if (!is_valid_ether_addr(addr))
		return;

	/*
	 * Since this is board-specific code, we'll cheat and use the
	 * physical address directly as we happen to know that it's
	 * the same as the virtual address.
	 */
	regs = (void __iomem __force *)res->start;
	pclk = clk_get(&pdev->dev, "pclk");
	if (!pclk)
		return;

	clk_enable(pclk);
	__raw_writel((addr[3] << 24) | (addr[2] << 16)
		     | (addr[1] << 8) | addr[0], regs + 0x98);
	__raw_writel((addr[5] << 8) | addr[4], regs + 0x9c);
	clk_disable(pclk);
	clk_put(pclk);
}


static int __init icnova_setup_mac(void) {
	set_hw_addr(at32_add_device_eth(0, &eth_data[0]));
#ifdef CONFIG_BOARD_ICNOVA_DEV_MACB
	set_hw_addr(at32_add_device_eth(1, &eth_data[1]));
#endif
	return 0;
}

arch_initcall(icnova_setup_mac);
