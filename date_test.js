const now = Date.now();
const testStr1 = "2026-03-21T22:42:02";
const testStr2 = "2026-03-21T22:42:02Z";
const testStr3 = "2026-03-21T22:42:02.123456+00:00";

console.log("Local time offset:", new Date().getTimezoneOffset());
console.log("No Z:", new Date(testStr1).getTime());
console.log("With Z:", new Date(testStr2).getTime());
console.log("Supabase exact format:", new Date(testStr3).getTime());
