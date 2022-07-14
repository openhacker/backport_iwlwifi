Patch  for problems with NUC NUC11TNKv5 with AX201 wifi for hostapd mode.

This is tested primarily with ubuntu 18.04 (kernel 5.4).

There is a branch (marty-backport) from git://git.kernel.org/pub/scm/linux/kernel/git/iwlwifi/backport-iwlwifi.git 

In client mode, the backport drivers work well (the distributed driver throws continuous "Unhandled alg" messages.

In hostapd, these messages appear constantly (but it basically works).
Added a module configuration to turn off these messages (in iwlwifi and iwlmvm its called "print_unhandled_alg".

# Building archive

Checkout marty-backport branch.

Follow instructions on:
    https://wireless.wiki.kernel.org/en/users/drivers/iwlwifi/core_release






    make defconfig-iwlwifi-public
    sed -i 's/CPTCFG_IWLMVM_VENDOR_CMDS=y/# CPTCFG_IWLMVM_VENDOR_CMDS is not set/' .config
    make -j4
    sudo make install
