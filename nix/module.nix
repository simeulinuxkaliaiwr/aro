self:
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.aro;
in
{
  options.programs.aro = {
    enable = lib.mkEnableOption "aro, a tiling Wayland compositor";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "aro.packages.\${system}.default";
      description = "The aro package to use.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];

    # listed by display managers
    services.displayManager.sessionPackages = [ cfg.package ];

    # screen sharing and screenshots through wlr, file pickers through gtk
    xdg.portal = {
      enable = true;
      wlr.enable = true;
      extraPortals = [ pkgs.xdg-desktop-portal-gtk ];
      configPackages = [ cfg.package ];
    };

    security.polkit.enable = lib.mkDefault true;
    hardware.graphics.enable = lib.mkDefault true;
  };
}
