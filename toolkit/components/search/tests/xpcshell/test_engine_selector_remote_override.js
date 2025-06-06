/* Any copyright is dedicated to the Public Domain.
 *    http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  SearchEngineSelector:
    "moz-src:///toolkit/components/search/SearchEngineSelector.sys.mjs",
});

const TEST_CONFIG = [
  {
    base: {
      urls: {
        search: {
          base: "https://www.bing.com/search",
          params: [
            {
              name: "old_param",
              value: "old_value",
            },
            {
              name: "old_code",
              value: "{partnerCode}",
            },
          ],
          searchTermParamName: "q",
        },
      },
      partnerCode: "old_partner_code",
    },
    variants: [
      {
        environment: {
          locales: ["en-US"],
        },
      },
    ],
    identifier: "aol",
    recordType: "engine",
  },
  {
    recordType: "defaultEngines",
    globalDefault: "aol",
    specificDefaults: [],
  },
  {
    orders: [],
    recordType: "engineOrders",
  },
];

const TEST_CONFIG_OVERRIDE = [
  {
    identifier: "aol",
    urls: {
      search: {
        params: [
          { name: "new_param", value: "new_value" },
          { name: "new_code", value: "{partnerCode}" },
        ],
      },
    },
    partnerCode: "new_partner_code",
    telemetrySuffix: "tsfx",
    clickUrl: "https://aol.url",
  },
];

const engineSelector = new SearchEngineSelector();

add_setup(async function () {
  SearchTestUtils.setRemoteSettingsConfig(TEST_CONFIG, TEST_CONFIG_OVERRIDE);
});

add_task(async function test_engine_selector() {
  let { engines } = await engineSelector.fetchEngineConfiguration({
    locale: "en-US",
    region: "us",
  });
  Assert.equal(engines[0].telemetrySuffix, "tsfx");
  Assert.equal(engines[0].clickUrl, "https://aol.url");
  Assert.equal(engines[0].urls.search.params[0].name, "new_param");
  Assert.equal(engines[0].urls.search.params[0].value, "new_value");
  Assert.equal(engines[0].urls.search.params[1].name, "new_code");
  Assert.equal(engines[0].urls.search.params[1].value, "{partnerCode}");
  Assert.equal(engines[0].partnerCode, "new_partner_code");
});
