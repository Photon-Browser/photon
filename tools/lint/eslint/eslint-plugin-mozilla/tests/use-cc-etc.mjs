/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// ------------------------------------------------------------------------------
// Requirements
// ------------------------------------------------------------------------------

import rule from "../lib/rules/use-cc-etc.mjs";
import { RuleTester } from "eslint";

const ruleTester = new RuleTester();

// ------------------------------------------------------------------------------
// Tests
// ------------------------------------------------------------------------------

function invalidCode(code, originalName, newName, output) {
  return {
    code,
    output,
    errors: [
      {
        messageId: "useCcEtc",
        data: {
          shortName: newName,
          oldName: originalName,
        },
        type: "MemberExpression",
      },
    ],
  };
}

ruleTester.run("use-cc-etc", rule, {
  valid: ["Components.Constructor();", "let x = Components.foo;"],
  invalid: [
    invalidCode(
      "let foo = Components.classes['bar'];",
      "classes",
      "Cc",
      "let foo = Cc['bar'];"
    ),
    invalidCode(
      "let bar = Components.interfaces.bar;",
      "interfaces",
      "Ci",
      "let bar = Ci.bar;"
    ),
    invalidCode(
      "Components.results.NS_ERROR_ILLEGAL_INPUT;",
      "results",
      "Cr",
      "Cr.NS_ERROR_ILLEGAL_INPUT;"
    ),
    invalidCode(
      "Components.utils.reportError('fake');",
      "utils",
      "Cu",
      "Cu.reportError('fake');"
    ),
  ],
});
