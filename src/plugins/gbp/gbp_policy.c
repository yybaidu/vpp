/*
 * Copyright (c) 2018 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <plugins/gbp/gbp.h>

/**
 * Grouping of global data for the GBP source EPG classification feature
 */
typedef struct gbp_policy_main_t_
{
  /**
   * Next nodes for L2 output features
   */
  u32 l2_output_feat_next[32];
} gbp_policy_main_t;

static gbp_policy_main_t gbp_policy_main;

#define foreach_gbp_policy                      \
  _(DENY,    "deny")

typedef enum
{
#define _(sym,str) GBP_ERROR_##sym,
  foreach_gbp_policy
#undef _
    GBP_POLICY_N_ERROR,
} gbp_policy_error_t;

static char *gbp_policy_error_strings[] = {
#define _(sym,string) string,
  foreach_gbp_policy
#undef _
};

typedef enum
{
#define _(sym,str) GBP_POLICY_NEXT_##sym,
  foreach_gbp_policy
#undef _
    GBP_POLICY_N_NEXT,
} gbp_policy_next_t;

/**
 * per-packet trace data
 */
typedef struct gbp_policy_trace_t_
{
  /* per-pkt trace data */
  epg_id_t src_epg;
  epg_id_t dst_epg;
  u32 acl_index;
} gbp_policy_trace_t;

static uword
gbp_policy (vlib_main_t * vm,
	    vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  gbp_main_t *gm = &gbp_main;
  gbp_policy_main_t *gpm = &gbp_policy_main;
  u32 n_left_from, *from, *to_next;
  u32 next_index;

  next_index = 0;
  n_left_from = frame->n_vectors;
  from = vlib_frame_vector_args (frame);

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  const gbp_endpoint_t *gep0;
	  gbp_policy_next_t next0;
	  gbp_contract_key_t key0;
	  gbp_contract_value_t value0 = {
	    .as_u64 = ~0,
	  };
	  u32 bi0, sw_if_index0;
	  vlib_buffer_t *b0;

	  next0 = GBP_POLICY_NEXT_DENY;
	  bi0 = from[0];
	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  b0 = vlib_get_buffer (vm, bi0);

	  /*
	   * determine the src and dst EPG
	   */
	  sw_if_index0 = vnet_buffer (b0)->sw_if_index[VLIB_TX];
	  gep0 = gbp_endpoint_get_itf (sw_if_index0);
	  key0.gck_dst = gep0->ge_epg_id;
	  key0.gck_src = vnet_buffer2 (b0)->gbp.src_epg;

	  if (EPG_INVALID != key0.gck_src)
	    {
	      if (PREDICT_FALSE (key0.gck_src == key0.gck_dst))
		{
		  /*
		   * intra-epg allowed
		   */
		  next0 = vnet_l2_feature_next (b0, gpm->l2_output_feat_next,
						L2OUTPUT_FEAT_GBP_POLICY);
		}
	      else
		{
		  value0.as_u64 = gbp_acl_lookup (&key0);

		  if (~0 != value0.gc_lc_index)
		    {
		      fa_5tuple_opaque_t pkt_5tuple0;
		      u8 action0 = 0;
		      u32 acl_pos_p0, acl_match_p0;
		      u32 rule_match_p0, trace_bitmap0;
		      u8 *h0, l2_len0;
		      u16 ether_type0;
		      u8 is_ip60 = 0;

		      l2_len0 = vnet_buffer (b0)->l2.l2_len;
		      h0 = vlib_buffer_get_current (b0);

		      ether_type0 =
			clib_net_to_host_u16 (*(u16 *) (h0 + l2_len0 - 2));

		      is_ip60 = (ether_type0 == ETHERNET_TYPE_IP6) ? 1 : 0;
		      /*
		       * tests against the ACL
		       */
		      acl_plugin_fill_5tuple_inline (gm->
						     acl_plugin.p_acl_main,
						     value0.gc_lc_index, b0,
						     is_ip60,
						     /* is_input */ 0,
						     /* is_l2_path */ 1,
						     &pkt_5tuple0);
		      acl_plugin_match_5tuple_inline (gm->
						      acl_plugin.p_acl_main,
						      value0.gc_lc_index,
						      &pkt_5tuple0, is_ip60,
						      &action0, &acl_pos_p0,
						      &acl_match_p0,
						      &rule_match_p0,
						      &trace_bitmap0);

		      if (action0 > 0)
			next0 =
			  vnet_l2_feature_next (b0, gpm->l2_output_feat_next,
						L2OUTPUT_FEAT_GBP_POLICY);
		    }
		}
	    }
	  else
	    {
	      /*
	       * the src EPG is not set when the packet arrives on an EPG
	       * uplink interface and we do not need to apply policy
	       */
	      next0 = vnet_l2_feature_next (b0, gpm->l2_output_feat_next,
					    L2OUTPUT_FEAT_GBP_POLICY);
	    }

	  if (PREDICT_FALSE ((b0->flags & VLIB_BUFFER_IS_TRACED)))
	    {
	      gbp_policy_trace_t *t =
		vlib_add_trace (vm, node, b0, sizeof (*t));
	      t->src_epg = key0.gck_src;
	      t->dst_epg = key0.gck_dst;
	      t->acl_index = value0.gc_acl_index;
	    }

	  /* verify speculative enqueue, maybe switch current next frame */
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return frame->n_vectors;
}

/* packet trace format function */
static u8 *
format_gbp_policy_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  gbp_policy_trace_t *t = va_arg (*args, gbp_policy_trace_t *);

  s =
    format (s, "src:%d, dst:%d, acl:%d", t->src_epg, t->dst_epg,
	    t->acl_index);

  return s;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (gbp_policy_node) = {
  .function = gbp_policy,
  .name = "gbp-policy",
  .vector_size = sizeof (u32),
  .format_trace = format_gbp_policy_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,

  .n_errors = ARRAY_LEN(gbp_policy_error_strings),
  .error_strings = gbp_policy_error_strings,

  .n_next_nodes = GBP_POLICY_N_NEXT,

  .next_nodes = {
    [GBP_POLICY_NEXT_DENY] = "error-drop",
  },
};

VLIB_NODE_FUNCTION_MULTIARCH (gbp_policy_node, gbp_policy);

/* *INDENT-ON* */

static clib_error_t *
gbp_policy_init (vlib_main_t * vm)
{
  gbp_policy_main_t *gpm = &gbp_policy_main;
  clib_error_t *error = 0;

  /* Initialize the feature next-node indexes */
  feat_bitmap_init_next_nodes (vm,
			       gbp_policy_node.index,
			       L2OUTPUT_N_FEAT,
			       l2output_get_feat_names (),
			       gpm->l2_output_feat_next);

  return error;
}

VLIB_INIT_FUNCTION (gbp_policy_init);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
