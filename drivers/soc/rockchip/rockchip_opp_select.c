// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal Rockchip OPP glue for vendor MPP/devfreq users.
 *
 * The full downstream driver also handles leakage, PVTM and read-margin
 * binning.  Mainline 7.0 does not carry those Rockchip internals, but MPP
 * still needs a real implementation instead of the header stubs so its OPP
 * table, clocks and supplies can be registered with the generic OPP core.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>

#include <soc/rockchip/rockchip_opp_select.h>

void rockchip_get_opp_data(const struct of_device_id *matches,
			   struct rockchip_opp_info *info)
{
	const struct of_device_id *match;

	if (!matches || !info || !info->dev)
		return;

	match = of_match_node(matches, of_root);
	if (!match)
		match = of_match_device(matches, info->dev);

	if (match)
		info->data = match->data;
}
EXPORT_SYMBOL(rockchip_get_opp_data);

void rockchip_opp_dvfs_lock(struct rockchip_opp_info *info)
{
	if (info)
		mutex_lock(&info->dvfs_mutex);
}
EXPORT_SYMBOL(rockchip_opp_dvfs_lock);

void rockchip_opp_dvfs_unlock(struct rockchip_opp_info *info)
{
	if (info)
		mutex_unlock(&info->dvfs_mutex);
}
EXPORT_SYMBOL(rockchip_opp_dvfs_unlock);

static int rockchip_opp_set_supplies(struct device *dev,
				     struct dev_pm_opp *old_opp,
				     struct dev_pm_opp *new_opp,
				     struct regulator **regulators,
				     unsigned int count)
{
	struct dev_pm_opp_supply *supplies;
	int ret = 0;
	unsigned int i;

	if (!new_opp || !count)
		return 0;

	supplies = kcalloc(count, sizeof(*supplies), GFP_KERNEL);
	if (!supplies)
		return -ENOMEM;

	ret = dev_pm_opp_get_supplies(new_opp, supplies);
	if (ret)
		goto out;

	for (i = 0; i < count; i++) {
		ret = regulator_set_voltage_triplet(regulators[i],
						    supplies[i].u_volt_min,
						    supplies[i].u_volt,
						    supplies[i].u_volt_max);
		if (ret) {
			dev_err(dev, "failed to set OPP supply %u voltage\n", i);
			goto out;
		}
	}

out:
	kfree(supplies);

	return ret;
}

int rockchip_init_opp_info(struct device *dev, struct rockchip_opp_info *info,
			   char *clk_name, char *reg_name)
{
	struct device_node *np;
	struct dev_pm_opp_config config = {};
	const char *clk_names[2];
	const char *reg_names[3];
	int ret;

	np = of_parse_phandle(dev->of_node, "operating-points-v2", 0);
	if (!np)
		return -ENOENT;
	of_node_put(np);

	info->dev = dev;
	info->bin = -EINVAL;
	info->process = -EINVAL;
	info->volt_sel = -EINVAL;
	info->pvtpll_clk_id = UINT_MAX;
	info->pvtpll_smc = true;
	info->is_runtime_active = true;
	mutex_init(&info->dvfs_mutex);
	clk_names[0] = NULL;
	clk_names[1] = NULL;
	reg_names[0] = NULL;
	reg_names[1] = NULL;
	reg_names[2] = NULL;

	if (clk_name) {
		clk_names[0] = clk_name;
		config.clk_names = clk_names;
	}

	if (reg_name) {
		reg_names[0] = reg_name;
		if (of_find_property(dev->of_node, "mem-supply", NULL)) {
			reg_names[1] = "mem";
			config.config_regulators = rockchip_opp_set_supplies;
		}
		config.regulator_names = reg_names;
	}

	if (info->data && info->data->config_clks)
		config.config_clks = info->data->config_clks;
	if (info->data && info->data->config_regulators)
		config.config_regulators = info->data->config_regulators;

	info->opp_token = dev_pm_opp_set_config(dev, &config);
	if (info->opp_token < 0) {
		ret = info->opp_token;
		dev_err(dev, "failed to set OPP config: %d\n", ret);

		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_init_opp_info);

void rockchip_uninit_opp_info(struct device *dev, struct rockchip_opp_info *info)
{
	if (info && info->opp_token > 0) {
		dev_pm_opp_clear_config(info->opp_token);
		info->opp_token = 0;
	}
}
EXPORT_SYMBOL(rockchip_uninit_opp_info);

int rockchip_adjust_opp_table(struct device *dev, struct rockchip_opp_info *info)
{
	return 0;
}
EXPORT_SYMBOL(rockchip_adjust_opp_table);

int rockchip_init_opp_table(struct device *dev, struct rockchip_opp_info *info,
			    char *clk_name, char *reg_name)
{
	int ret;

	ret = rockchip_init_opp_info(dev, info, clk_name, reg_name);
	if (ret)
		return ret;

	ret = dev_pm_opp_of_add_table(dev);
	if (ret) {
		dev_err(dev, "failed to add OPP table: %d\n", ret);
		rockchip_uninit_opp_info(dev, info);

		return ret;
	}

	rockchip_adjust_opp_table(dev, info);

	return 0;
}
EXPORT_SYMBOL(rockchip_init_opp_table);

void rockchip_uninit_opp_table(struct device *dev, struct rockchip_opp_info *info)
{
	dev_pm_opp_of_remove_table(dev);
	rockchip_uninit_opp_info(dev, info);
}
EXPORT_SYMBOL(rockchip_uninit_opp_table);

int rockchip_of_get_leakage(struct device *dev, char *lkg_name, int *leakage)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(rockchip_of_get_leakage);

int rockchip_nvmem_cell_read_u8(struct device_node *np, const char *cell_id,
				u8 *val)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(rockchip_nvmem_cell_read_u8);

int rockchip_nvmem_cell_read_u16(struct device_node *np, const char *cell_id,
				 u16 *val)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(rockchip_nvmem_cell_read_u16);

int rockchip_get_read_margin(struct device *dev,
			     struct rockchip_opp_info *info,
			     unsigned long volt, u32 *target_rm)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(rockchip_get_read_margin);

int rockchip_set_read_margin(struct device *dev,
			     struct rockchip_opp_info *info, u32 rm,
			     bool is_set_rm)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(rockchip_set_read_margin);

int rockchip_set_intermediate_rate(struct device *dev,
				   struct rockchip_opp_info *info,
				   struct clk *clk, unsigned long old_freq,
				   unsigned long new_freq, bool is_scaling_up,
				   bool is_set_clk)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(rockchip_set_intermediate_rate);

int rockchip_opp_set_low_length(struct device *dev, struct device_node *np,
				struct rockchip_opp_info *opp_info)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(rockchip_opp_set_low_length);

int rockchip_opp_config_regulators(struct device *dev,
				   struct dev_pm_opp *old_opp,
				   struct dev_pm_opp *new_opp,
				   struct regulator **regulators,
				   unsigned int count,
				   struct rockchip_opp_info *info)
{
	return rockchip_opp_set_supplies(dev, old_opp, new_opp,
					 regulators, count);
}
EXPORT_SYMBOL(rockchip_opp_config_regulators);

int rockchip_opp_config_clks(struct device *dev, struct opp_table *opp_table,
			     struct dev_pm_opp *opp, void *data,
			     bool scaling_down, struct rockchip_opp_info *info)
{
	return dev_pm_opp_config_clks_simple(dev, opp_table, opp, data,
					     scaling_down);
}
EXPORT_SYMBOL(rockchip_opp_config_clks);

int rockchip_opp_check_rate_volt(struct device *dev,
				 struct rockchip_opp_info *info)
{
	return 0;
}
EXPORT_SYMBOL(rockchip_opp_check_rate_volt);

MODULE_DESCRIPTION("Rockchip OPP glue");
MODULE_LICENSE("GPL");
