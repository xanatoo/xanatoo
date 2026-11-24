# Reply composer

The native discussion readers (Hacker News, Reddit, Lemmy, Nostr) display
comments as semantic `Message` cards. Right-click a comment card and select
**Reply…** to open the Reply panel. This also works with local documents
containing `Thread` / `Message` nodes.

For an offline test, open `examples/reply-demo.xml` and right-click any of its
three comments. The Reply panel is a standard Xanatoo floating window: drag
its title to move/dock it, resize it from its edges, or close it with the X.
When a dragged panel snaps to the window edge, its editor follows the same
snapped geometry as the frame. Labels and controls wrap in narrow side docks;
a short panel can scroll to reach the Send button.

- The composer accepts multiline text and has **Bold**, **Italic**, **Code**,
  **Link**, and **Quote** buttons. They insert Markdown delimiters around
  the selected text, or at the cursor if nothing is selected.
- The comment being replied to has an amber wash and border in the document,
  and a matching amber scrollbar tick (including when the message leaves the
  active virtual-layout band).
- **Nostr notes** (a 64-hex message label on a `nostr:` page): Send posts
  for real, as the unlocked active account (Identities → Nostr). The panel
  names the account, or says in red why it cannot send (no account, locked,
  watch-only, no signing in this build) and keeps Send disabled. Nostr
  notes are plain text, so only the Quote button is offered. A worker
  (`send_reply` → `nostr_reply`) finds the parent (local cache, else the
  group's relay and the default relays by id) and lets `nostr_reply_plan`
  pick the answer by the parent's kind:
  - kind 1 note → kind 1 with NIP-10 marked `root` / `reply` e tags and
    `p` tags for the parent's author and everyone it tagged;
  - kind 9 group / chat message → kind 9 quoting it (`q`, NIP-C7) in the
    same group (`h`);
  - kind 1111 comment → kind 1111 under the same root (its uppercase tags);
  - anything else with content (kind 11 group threads, articles, pictures,
    …) → a NIP-22 kind-1111 comment rooted at it (`A` for addressable
    events), keeping the group's `h`;
  - profiles, lists, reactions, reposts, deletions, private messages, zaps
    and ephemeral events are refused with a message.

  `nostr:npub` mentions add `p` tags. Group replies go only to the group's
  relay (reply from the group page); the rest to the default publish relays. On success the composer empties and the views
  showing the page reload; the outcome stays under the Send button.
- **Other services:** Send deliberately does not contact anything. It
  reports that no account is connected, leaving the draft visible; the
  title says "Reply (mockup)".
- The draft is session-only, scoped to the selected comment; choosing another
  comment starts a new draft.

The main `App::reply` record owns the target and draft; `Ui` retains only the
ImGui editor's transient text/selection state. The target is identified by
buffer and message ID/label, not by the caret or current text selection.
The scrollbar marker remembers the last known document Y when the target
card is virtualised out of the active layout. Closing the panel removes both
highlights.
