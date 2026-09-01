const { FusesPlugin } = require('@electron-forge/plugin-fuses');
const { FuseV1Options, FuseVersion } = require('@electron/fuses');
const path = require('node:path');

module.exports = {
  packagerConfig: {
    asar: { unpack: '**/*.{node,dll}' },
    // The MinGW N-API import library binds to electron.exe. Keep the internal
    // executable name stable; Squirrel still brands the package ContextReader.
    executableName: 'electron',
    name: 'ContextReader',
    electronZipDir: path.join(__dirname, 'build', 'electron-downloads'),
    ignore: (filePath) => {
      const value = filePath.replaceAll('\\', '/');
      if (!value) return false;
      if (value === '/package.json' || value === '/package-lock.json') return false;
      if (value === '/hosts' || value.startsWith('/hosts/electron')) return false;
      if (value === '/node_modules' || value.startsWith('/node_modules/')) return false;
      if (value === '/build' || value === '/build/node-p2-ucrt64') return false;
      if (value === '/build/build-id.txt') return false;
      if (value === '/build/node-p2-ucrt64/reader_node.node') return false;
      if (value === '/build/node-p2-ucrt64/libwinpthread-1.dll') return false;
      return true;
    },
  },
  rebuildConfig: {},
  makers: [{
    name: '@electron-forge/maker-squirrel',
    config: {
      name: 'context_reader',
      authors: 'Context Reader',
      description: 'Local contextual PDF reader',
      noMsi: true,
    },
  }],
  plugins: [
    new FusesPlugin({
      version: FuseVersion.V1,
      [FuseV1Options.RunAsNode]: false,
      [FuseV1Options.EnableCookieEncryption]: true,
      [FuseV1Options.EnableNodeOptionsEnvironmentVariable]: false,
      [FuseV1Options.EnableNodeCliInspectArguments]: false,
      [FuseV1Options.EnableEmbeddedAsarIntegrityValidation]: true,
      [FuseV1Options.OnlyLoadAppFromAsar]: true,
      [FuseV1Options.GrantFileProtocolExtraPrivileges]: false,
    }),
  ],
};
