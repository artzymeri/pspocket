#!/usr/bin/env node
// App Store Connect, through its API with an API key (an ES256 token signed by the key's .p8 file; no Apple ID), for
// scripts/ios.sh testflight. The key and the token are never printed.
//
//   node scripts/asc.mjs profile BUNDLE_ID PROFILE_NAME CERT_SERIAL [CAPABILITY...]
//       The bundle ID gets the capabilities (API names, e.g. INCREASED_MEMORY_LIMIT), and an App Store profile named
//       PROFILE_NAME is made again with the Apple Distribution certificate of that serial (the one in this Mac's
//       keychain), then installed where Xcode looks. A profile only carries the capabilities its App ID had when it was
//       made, so it is made anew each time.
//   node scripts/asc.mjs testers BUNDLE_ID
//       An internal TestFlight group with access to every build (made if the app has none), holding the team's account
//       holder and admins; invitations go out once a build has been processed.
//   node scripts/asc.mjs builds BUNDLE_ID
//       The app's builds and where App Store Connect is with them.
//
// Environment: PSPOCKET_ASC_KEY_ID, PSPOCKET_ASC_ISSUER_ID, PSPOCKET_ASC_KEY_PATH (default
// ~/.appstoreconnect/private_keys/AuthKey_<ID>.p8). The app itself has to exist in App Store Connect first: the API
// cannot create apps.
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const fail = (msg) => {
  console.error(`asc: ${msg}`);
  process.exit(1);
};
const keyId = process.env.PSPOCKET_ASC_KEY_ID;
const issuer = process.env.PSPOCKET_ASC_ISSUER_ID;
if (!keyId || !issuer) fail('set PSPOCKET_ASC_KEY_ID and PSPOCKET_ASC_ISSUER_ID');
const keyPath = process.env.PSPOCKET_ASC_KEY_PATH || path.join(os.homedir(), `.appstoreconnect/private_keys/AuthKey_${keyId}.p8`);
const key = crypto.createPrivateKey(fs.readFileSync(keyPath));

// Ten minutes: Apple refuses tokens that live longer than twenty.
function token() {
  const b64 = (o) => Buffer.from(JSON.stringify(o)).toString('base64url');
  const now = Math.floor(Date.now() / 1000);
  const head = b64({ alg: 'ES256', kid: keyId, typ: 'JWT' });
  const body = b64({ iss: issuer, iat: now, exp: now + 600, aud: 'appstoreconnect-v1' });
  const sig = crypto.sign('sha256', Buffer.from(`${head}.${body}`), { key, dsaEncoding: 'ieee-p1363' });
  return `${head}.${body}.${sig.toString('base64url')}`;
}

async function api(method, endpoint, data) {
  const res = await fetch(`https://api.appstoreconnect.apple.com${endpoint}`, {
    method,
    headers: { Authorization: `Bearer ${token()}`, 'Content-Type': 'application/json' },
    body: data ? JSON.stringify({ data }) : undefined,
  });
  const json = res.status === 204 ? {} : await res.json().catch(() => ({}));
  if (!res.ok) {
    const why = json.errors?.map((e) => e.detail ?? e.title).join('; ') ?? '';
    throw new Error(`${method} ${endpoint}: HTTP ${res.status} ${why}`);
  }
  return json;
}

async function bundleIdOf(identifier) {
  const ids = await api('GET', `/v1/bundleIds?filter[identifier]=${encodeURIComponent(identifier)}`);
  const b = ids.data.find((x) => x.attributes.identifier === identifier);
  if (!b) fail(`bundle ID ${identifier} is not registered (a development build registers it)`);
  return b;
}

async function appOf(identifier) {
  const apps = await api('GET', `/v1/apps?filter[bundleId]=${encodeURIComponent(identifier)}`);
  if (!apps.data.length) {
    fail(`App Store Connect has no app for ${identifier}: create it once at https://appstoreconnect.apple.com ` +
      `(Apps > + > New App, that bundle ID)`);
  }
  return apps.data[0];
}

async function profile(identifier, name, serial, caps) {
  const b = await bundleIdOf(identifier);
  const have = await api('GET', `/v1/bundleIds/${b.id}/bundleIdCapabilities`);
  const present = new Set(have.data.map((c) => c.attributes.capabilityType));
  for (const cap of caps) {
    if (present.has(cap)) continue;
    await api('POST', '/v1/bundleIdCapabilities', { type: 'bundleIdCapabilities', attributes: { capabilityType: cap },
      relationships: { bundleId: { data: { type: 'bundleIds', id: b.id } } } });
    console.log(`asc: capability ${cap} turned on for ${identifier}`);
  }
  const certs = await api('GET', '/v1/certificates?limit=200');
  const norm = (s) => s.replace(/^0+/, '').toUpperCase();
  const cert = certs.data.find((c) => c.attributes.certificateType === 'DISTRIBUTION' &&
    norm(c.attributes.serialNumber) === norm(serial));
  if (!cert) fail(`no Apple Distribution certificate with serial ${serial} in the team`);
  const old = await api('GET', `/v1/bundleIds/${b.id}/profiles?limit=200`);
  for (const p of old.data.filter((p) => p.attributes.name === name)) await api('DELETE', `/v1/profiles/${p.id}`);
  const made = await api('POST', '/v1/profiles', { type: 'profiles',
    attributes: { name, profileType: 'IOS_APP_STORE' },
    relationships: { bundleId: { data: { type: 'bundleIds', id: b.id } },
      certificates: { data: [{ type: 'certificates', id: cert.id }] } } });
  const attrs = made.data.attributes;
  const dir = path.join(os.homedir(), 'Library/Developer/Xcode/UserData/Provisioning Profiles');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, `${attrs.uuid}.mobileprovision`), Buffer.from(attrs.profileContent, 'base64'));
  console.log(`asc: profile "${attrs.name}" (${attrs.uuid}) installed, certificate ${cert.attributes.name}`);
}

async function testers(identifier) {
  const app = await appOf(identifier);
  const groups = await api('GET', `/v1/apps/${app.id}/betaGroups`);
  let group = groups.data.find((g) => g.attributes.isInternalGroup);
  if (!group) {
    group = (await api('POST', '/v1/betaGroups', { type: 'betaGroups',
      attributes: { name: 'Internal', isInternalGroup: true, hasAccessToAllBuilds: true },
      relationships: { app: { data: { type: 'apps', id: app.id } } } })).data;
    console.log('asc: internal TestFlight group "Internal" made');
  }
  const members = await api('GET', `/v1/betaGroups/${group.id}/betaTesters?limit=200`);
  const known = new Set(members.data.map((t) => t.attributes.email?.toLowerCase()));
  const users = await api('GET', '/v1/users?limit=200');
  for (const u of users.data) {
    if (!u.attributes.roles.some((r) => r === 'ACCOUNT_HOLDER' || r === 'ADMIN')) continue;
    const email = u.attributes.username;
    if (known.has(email.toLowerCase())) continue;
    await api('POST', '/v1/betaTesters', { type: 'betaTesters',
      attributes: { email, firstName: u.attributes.firstName, lastName: u.attributes.lastName },
      relationships: { betaGroups: { data: [{ type: 'betaGroups', id: group.id }] } } });
    console.log(`asc: ${email} added to "${group.attributes.name}"`);
  }
  // Invitations, once a build is ready: an internal tester gets each processed build of a group with all builds.
  const builds = await api('GET', `/v1/builds?filter[app]=${app.id}&filter[processingState]=VALID&limit=1`);
  if (!builds.data.length) {
    console.log('asc: no processed build yet: TestFlight shows it to the group once App Store Connect is done');
    return;
  }
  const now = await api('GET', `/v1/betaGroups/${group.id}/betaTesters?limit=200`);
  for (const t of now.data) {
    try {
      await api('POST', '/v1/betaTesterInvitations', { type: 'betaTesterInvitations', relationships: {
        app: { data: { type: 'apps', id: app.id } }, betaTester: { data: { type: 'betaTesters', id: t.id } } } });
      console.log(`asc: TestFlight invitation sent to ${t.attributes.email}`);
    } catch (e) {
      console.log(`asc: ${t.attributes.email}: ${e.message}`);
    }
  }
}

async function builds(identifier) {
  const app = await appOf(identifier);
  const list = await api('GET', `/v1/builds?filter[app]=${app.id}&sort=-uploadedDate&limit=10`);
  if (!list.data.length) console.log('asc: no builds yet (an upload takes a few minutes to show up)');
  for (const b of list.data) console.log(`asc: build ${b.attributes.version}: ${b.attributes.processingState} (uploaded ${b.attributes.uploadedDate})`);
}

const [command, ...args] = process.argv.slice(2);
try {
  if (command === 'profile' && args.length >= 3) await profile(args[0], args[1], args[2], args.slice(3));
  else if (command === 'testers' && args.length === 1) await testers(args[0]);
  else if (command === 'builds' && args.length === 1) await builds(args[0]);
  else fail('usage: asc.mjs profile BUNDLE_ID NAME CERT_SERIAL [CAP...] | testers BUNDLE_ID | builds BUNDLE_ID');
} catch (e) {
  fail(e.message);
}
