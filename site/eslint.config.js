import {defineConfig, globalIgnores} from "eslint/config";
import tsPlugin from "@typescript-eslint/eslint-plugin";
import tsParser from "@typescript-eslint/parser";
import reactHooks from "eslint-plugin-react-hooks";

const eslintConfig = defineConfig([
  globalIgnores([".next/**", "out/**", "build/**", "node_modules/**"]),
  {
    files: ["**/*.{ts,tsx}"],
    languageOptions: {
      parser: tsParser,
      parserOptions: {
        ecmaVersion: "latest",
        sourceType: "module",
        ecmaFeatures: {jsx: true},
      },
    },
    plugins: {
      "@typescript-eslint": tsPlugin,
      "react-hooks": reactHooks,
    },
    rules: {
      ...reactHooks.configs.recommended.rules,
      curly: ["error", "all"],
      "@typescript-eslint/array-type": ["error", {default: "generic"}],
      "@typescript-eslint/no-unused-vars": ["error", {argsIgnorePattern: "^_"}],
    },
  },
]);

export default eslintConfig;
